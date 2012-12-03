//===- LiveDebugVariables.cpp - Tracking debug info variables -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LiveDebugVariables analysis.
//
// Remove all DBG_VALUE instructions referencing virtual registers and replace
// them with a data structure tracking where live user variables are kept - in a
// virtual register or in a stack slot.
//
// Allow the data structure to be updated during register allocation when values
// are moved between registers and stack slots. Finally emit new DBG_VALUE
// instructions after register allocation is complete.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "livedebug"
#include "LiveDebugVariables.h"
#include "VirtRegMap.h"
#include "llvm/Constants.h"
#include "llvm/DebugInfo.h"
#include "llvm/Metadata.h"
#include "llvm/Value.h"
#include "llvm/ADT/IntervalMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LexicalScopes.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"

using namespace llvm;

static cl::opt<bool>
EnableLDV("live-debug-variables", cl::init(true),
          cl::desc("Enable the live debug variables pass"), cl::Hidden);

STATISTIC(NumInsertedDebugValues, "Number of DBG_VALUEs inserted");
char LiveDebugVariables::ID = 0;

INITIALIZE_PASS_BEGIN(LiveDebugVariables, "livedebugvars",
                "Debug Variable Analysis", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_END(LiveDebugVariables, "livedebugvars",
                "Debug Variable Analysis", false, false)

void LiveDebugVariables::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MachineDominatorTree>();
  AU.addRequiredTransitive<LiveIntervals>();
  AU.setPreservesAll();
  MachineFunctionPass::getAnalysisUsage(AU);
}

LiveDebugVariables::LiveDebugVariables() : MachineFunctionPass(ID), pImpl(0) {
  initializeLiveDebugVariablesPass(*PassRegistry::getPassRegistry());
}

/// LocMap - Map of where a user value is live, and its location.
typedef IntervalMap<SlotIndex, unsigned, 4> LocMap;

namespace {
/// UserValueScopes - Keeps track of lexical scopes associated with an
/// user value's source location.
class UserValueScopes {
  DebugLoc DL;
  LexicalScopes &LS;
  SmallPtrSet<const MachineBasicBlock *, 4> LBlocks;

public:
  UserValueScopes(DebugLoc D, LexicalScopes &L) : DL(D), LS(L) {}

  /// dominates - Return true if current scope dominates at least one machine
  /// instruction in a given machine basic block.
  bool dominates(MachineBasicBlock *MBB) {
    if (LBlocks.empty())
      LS.getMachineBasicBlocks(DL, LBlocks);
    if (LBlocks.count(MBB) != 0 || LS.dominates(DL, MBB))
      return true;
    return false;
  }
};
} // end anonymous namespace

/// UserValue - A user value is a part of a debug info user variable.
///
/// A DBG_VALUE instruction notes that (a sub-register of) a virtual register
/// holds part of a user variable. The part is identified by a byte offset.
///
/// UserValues are grouped into equivalence classes for easier searching. Two
/// user values are related if they refer to the same variable, or if they are
/// held by the same virtual register. The equivalence class is the transitive
/// closure of that relation.
namespace {
class LDVImpl;
class UserValue {
  const MDNode *variable; ///< The debug info variable we are part of.
  unsigned offset;        ///< Byte offset into variable.
  DebugLoc dl;            ///< The debug location for the variable. This is
                          ///< used by dwarf writer to find lexical scope.
  UserValue *leader;      ///< Equivalence class leader.
  UserValue *next;        ///< Next value in equivalence class, or null.

  /// Numbered locations referenced by locmap.
  SmallVector<MachineOperand, 4> locations;

  /// Map of slot indices where this value is live.
  LocMap locInts;

  /// coalesceLocation - After LocNo was changed, check if it has become
  /// identical to another location, and coalesce them. This may cause LocNo or
  /// a later location to be erased, but no earlier location will be erased.
  void coalesceLocation(unsigned LocNo);

  /// insertDebugValue - Insert a DBG_VALUE into MBB at Idx for LocNo.
  void insertDebugValue(MachineBasicBlock *MBB, SlotIndex Idx, unsigned LocNo,
                        LiveIntervals &LIS, const TargetInstrInfo &TII);

  /// splitLocation - Replace OldLocNo ranges with NewRegs ranges where NewRegs
  /// is live. Returns true if any changes were made.
  bool splitLocation(unsigned OldLocNo, ArrayRef<LiveInterval*> NewRegs);

public:
  /// UserValue - Create a new UserValue.
  UserValue(const MDNode *var, unsigned o, DebugLoc L,
            LocMap::Allocator &alloc)
    : variable(var), offset(o), dl(L), leader(this), next(0), locInts(alloc)
  {}

  /// getLeader - Get the leader of this value's equivalence class.
  UserValue *getLeader() {
    UserValue *l = leader;
    while (l != l->leader)
      l = l->leader;
    return leader = l;
  }

  /// getNext - Return the next UserValue in the equivalence class.
  UserValue *getNext() const { return next; }

  /// match - Does this UserValue match the parameters?
  bool match(const MDNode *Var, unsigned Offset) const {
    return Var == variable && Offset == offset;
  }

  /// merge - Merge equivalence classes.
  static UserValue *merge(UserValue *L1, UserValue *L2) {
    L2 = L2->getLeader();
    if (!L1)
      return L2;
    L1 = L1->getLeader();
    if (L1 == L2)
      return L1;
    // Splice L2 before L1's members.
    UserValue *End = L2;
    while (End->next)
      End->leader = L1, End = End->next;
    End->leader = L1;
    End->next = L1->next;
    L1->next = L2;
    return L1;
  }

  /// getLocationNo - Return the location number that matches Loc.
  unsigned getLocationNo(const MachineOperand &LocMO) {
    if (LocMO.isReg()) {
      if (LocMO.getReg() == 0)
        return ~0u;
      // For register locations we dont care about use/def and other flags.
      for (unsigned i = 0, e = locations.size(); i != e; ++i)
        if (locations[i].isReg() &&
            locations[i].getReg() == LocMO.getReg() &&
            locations[i].getSubReg() == LocMO.getSubReg())
          return i;
    } else
      for (unsigned i = 0, e = locations.size(); i != e; ++i)
        if (LocMO.isIdenticalTo(locations[i]))
          return i;
    locations.push_back(LocMO);
    // We are storing a MachineOperand outside a MachineInstr.
    locations.back().clearParent();
    // Don't store def operands.
    if (locations.back().isReg())
      locations.back().setIsUse();
    return locations.size() - 1;
  }

  /// mapVirtRegs - Ensure that all virtual register locations are mapped.
  void mapVirtRegs(LDVImpl *LDV);

  /// addDef - Add a definition point to this value.
  void addDef(SlotIndex Idx, const MachineOperand &LocMO) {
    // Add a singular (Idx,Idx) -> Loc mapping.
    LocMap::iterator I = locInts.find(Idx);
    if (!I.valid() || I.start() != Idx)
      I.insert(Idx, Idx.getNextSlot(), getLocationNo(LocMO));
    else
      // A later DBG_VALUE at the same SlotIndex overrides the old location.
      I.setValue(getLocationNo(LocMO));
  }

  /// extendDef - Extend the current definition as far as possible down the
  /// dominator tree. Stop when meeting an existing def or when leaving the live
  /// range of VNI.
  /// End points where VNI is no longer live are added to Kills.
  /// @param Idx   Starting point for the definition.
  /// @param LocNo Location number to propagate.
  /// @param LI    Restrict liveness to where LI has the value VNI. May be null.
  /// @param VNI   When LI is not null, this is the value to restrict to.
  /// @param Kills Append end points of VNI's live range to Kills.
  /// @param LIS   Live intervals analysis.
  /// @param MDT   Dominator tree.
  void extendDef(SlotIndex Idx, unsigned LocNo,
                 LiveInterval *LI, const VNInfo *VNI,
                 SmallVectorImpl<SlotIndex> *Kills,
                 LiveIntervals &LIS, MachineDominatorTree &MDT,
                 UserValueScopes &UVS);

  /// addDefsFromCopies - The value in LI/LocNo may be copies to other
  /// registers. Determine if any of the copies are available at the kill
  /// points, and add defs if possible.
  /// @param LI      Scan for copies of the value in LI->reg.
  /// @param LocNo   Location number of LI->reg.
  /// @param Kills   Points where the range of LocNo could be extended.
  /// @param NewDefs Append (Idx, LocNo) of inserted defs here.
  void addDefsFromCopies(LiveInterval *LI, unsigned LocNo,
                      const SmallVectorImpl<SlotIndex> &Kills,
                      SmallVectorImpl<std::pair<SlotIndex, unsigned> > &NewDefs,
                      MachineRegisterInfo &MRI,
                      LiveIntervals &LIS);

  /// computeIntervals - Compute the live intervals of all locations after
  /// collecting all their def points.
  void computeIntervals(MachineRegisterInfo &MRI, const TargetRegisterInfo &TRI,
                        LiveIntervals &LIS, MachineDominatorTree &MDT,
                        UserValueScopes &UVS);

  /// renameRegister - Update locations to rewrite OldReg as NewReg:SubIdx.
  void renameRegister(unsigned OldReg, unsigned NewReg, unsigned SubIdx,
                      const TargetRegisterInfo *TRI);

  /// splitRegister - Replace OldReg ranges with NewRegs ranges where NewRegs is
  /// live. Returns true if any changes were made.
  bool splitRegister(unsigned OldLocNo, ArrayRef<LiveInterval*> NewRegs);

  /// rewriteLocations - Rewrite virtual register locations according to the
  /// provided virtual register map.
  void rewriteLocations(VirtRegMap &VRM, const TargetRegisterInfo &TRI);

  /// emitDebugVariables - Recreate DBG_VALUE instruction from data structures.
  void emitDebugValues(VirtRegMap *VRM,
                       LiveIntervals &LIS, const TargetInstrInfo &TRI);

  /// findDebugLoc - Return DebugLoc used for this DBG_VALUE instruction. A
  /// variable may have more than one corresponding DBG_VALUE instructions. 
  /// Only first one needs DebugLoc to identify variable's lexical scope
  /// in source file.
  DebugLoc findDebugLoc();

  /// getDebugLoc - Return DebugLoc of this UserValue.
  DebugLoc getDebugLoc() { return dl;}
  void print(raw_ostream&, const TargetMachine*);
};
} // namespace

/// LDVImpl - Implementation of the LiveDebugVariables pass.
namespace {
class LDVImpl {
  LiveDebugVariables &pass;
  LocMap::Allocator allocator;
  MachineFunction *MF;
  LiveIntervals *LIS;
  LexicalScopes LS;
  MachineDominatorTree *MDT;
  const TargetRegisterInfo *TRI;

  /// userValues - All allocated UserValue instances.
  SmallVector<UserValue*, 8> userValues;

  /// Map virtual register to eq class leader.
  typedef DenseMap<unsigned, UserValue*> VRMap;
  VRMap virtRegToEqClass;

  /// Map user variable to eq class leader.
  typedef DenseMap<const MDNode *, UserValue*> UVMap;
  UVMap userVarMap;

  /// getUserValue - Find or create a UserValue.
  UserValue *getUserValue(const MDNode *Var, unsigned Offset, DebugLoc DL);

  /// lookupVirtReg - Find the EC leader for VirtReg or null.
  UserValue *lookupVirtReg(unsigned VirtReg);

  /// handleDebugValue - Add DBG_VALUE instruction to our maps.
  /// @param MI  DBG_VALUE instruction
  /// @param Idx Last valid SLotIndex before instruction.
  /// @return    True if the DBG_VALUE instruction should be deleted.
  bool handleDebugValue(MachineInstr *MI, SlotIndex Idx);

  /// collectDebugValues - Collect and erase all DBG_VALUE instructions, adding
  /// a UserValue def for each instruction.
  /// @param mf MachineFunction to be scanned.
  /// @return True if any debug values were found.
  bool collectDebugValues(MachineFunction &mf);

  /// computeIntervals - Compute the live intervals of all user values after
  /// collecting all their def points.
  void computeIntervals();

public:
  LDVImpl(LiveDebugVariables *ps) : pass(*ps) {}
  bool runOnMachineFunction(MachineFunction &mf);

  /// clear - Relase all memory.
  void clear() {
    DeleteContainerPointers(userValues);
    userValues.clear();
    virtRegToEqClass.clear();
    userVarMap.clear();
  }

  /// mapVirtReg - Map virtual register to an equivalence class.
  void mapVirtReg(unsigned VirtReg, UserValue *EC);

  /// renameRegister - Replace all references to OldReg with NewReg:SubIdx.
  void renameRegister(unsigned OldReg, unsigned NewReg, unsigned SubIdx);

  /// splitRegister -  Replace all references to OldReg with NewRegs.
  void splitRegister(unsigned OldReg, ArrayRef<LiveInterval*> NewRegs);

  /// emitDebugVariables - Recreate DBG_VALUE instruction from data structures.
  void emitDebugValues(VirtRegMap *VRM);

  void print(raw_ostream&);
};
} // namespace

void UserValue::print(raw_ostream &OS, const TargetMachine *TM) {
  DIVariable DV(variable);
  OS << "!\""; 
  DV.printExtendedName(OS);
  OS << "\"\t";
  if (offset)
    OS << '+' << offset;
  for (LocMap::const_iterator I = locInts.begin(); I.valid(); ++I) {
    OS << " [" << I.start() << ';' << I.stop() << "):";
    if (I.value() == ~0u)
      OS << "undef";
    else
      OS << I.value();
  }
  for (unsigned i = 0, e = locations.size(); i != e; ++i) {
    OS << " Loc" << i << '=';
    locations[i].print(OS, TM);
  }
  OS << '\n';
}

void LDVImpl::print(raw_ostream &OS) {
  OS << "********** DEBUG VARIABLES **********\n";
  for (unsigned i = 0, e = userValues.size(); i != e; ++i)
    userValues[i]->print(OS, &MF->getTarget());
}

void UserValue::coalesceLocation(unsigned LocNo) {
  unsigned KeepLoc = 0;
  for (unsigned e = locations.size(); KeepLoc != e; ++KeepLoc) {
    if (KeepLoc == LocNo)
      continue;
    if (locations[KeepLoc].isIdenticalTo(locations[LocNo]))
      break;
  }
  // No matches.
  if (KeepLoc == locations.size())
    return;

  // Keep the smaller location, erase the larger one.
  unsigned EraseLoc = LocNo;
  if (KeepLoc > EraseLoc)
    std::swap(KeepLoc, EraseLoc);
  locations.erase(locations.begin() + EraseLoc);

  // Rewrite values.
  for (LocMap::iterator I = locInts.begin(); I.valid(); ++I) {
    unsigned v = I.value();
    if (v == EraseLoc)
      I.setValue(KeepLoc);      // Coalesce when possible.
    else if (v > EraseLoc)
      I.setValueUnchecked(v-1); // Avoid coalescing with untransformed values.
  }
}

void UserValue::mapVirtRegs(LDVImpl *LDV) {
  for (unsigned i = 0, e = locations.size(); i != e; ++i)
    if (locations[i].isReg() &&
        TargetRegisterInfo::isVirtualRegister(locations[i].getReg()))
      LDV->mapVirtReg(locations[i].getReg(), this);
}

UserValue *LDVImpl::getUserValue(const MDNode *Var, unsigned Offset,
                                 DebugLoc DL) {
  UserValue *&Leader = userVarMap[Var];
  if (Leader) {
    UserValue *UV = Leader->getLeader();
    Leader = UV;
    for (; UV; UV = UV->getNext())
      if (UV->match(Var, Offset))
        return UV;
  }

  UserValue *UV = new UserValue(Var, Offset, DL, allocator);
  userValues.push_back(UV);
  Leader = UserValue::merge(Leader, UV);
  return UV;
}

void LDVImpl::mapVirtReg(unsigned VirtReg, UserValue *EC) {
  assert(TargetRegisterInfo::isVirtualRegister(VirtReg) && "Only map VirtRegs");
  UserValue *&Leader = virtRegToEqClass[VirtReg];
  Leader = UserValue::merge(Leader, EC);
}

UserValue *LDVImpl::lookupVirtReg(unsigned VirtReg) {
  if (UserValue *UV = virtRegToEqClass.lookup(VirtReg))
    return UV->getLeader();
  return 0;
}

bool LDVImpl::handleDebugValue(MachineInstr *MI, SlotIndex Idx) {
  // DBG_VALUE loc, offset, variable
  if (MI->getNumOperands() != 3 ||
      !MI->getOperand(1).isImm() || !MI->getOperand(2).isMetadata()) {
    DEBUG(dbgs() << "Can't handle " << *MI);
    return false;
  }

  // Get or create the UserValue for (variable,offset).
  unsigned Offset = MI->getOperand(1).getImm();
  const MDNode *Var = MI->getOperand(2).getMetadata();
  UserValue *UV = getUserValue(Var, Offset, MI->getDebugLoc());
  UV->addDef(Idx, MI->getOperand(0));
  return true;
}

bool LDVImpl::collectDebugValues(MachineFunction &mf) {
  bool Changed = false;
  for (MachineFunction::iterator MFI = mf.begin(), MFE = mf.end(); MFI != MFE;
       ++MFI) {
    MachineBasicBlock *MBB = MFI;
    for (MachineBasicBlock::iterator MBBI = MBB->begin(), MBBE = MBB->end();
         MBBI != MBBE;) {
      if (!MBBI->isDebugValue()) {
        ++MBBI;
        continue;
      }
      // DBG_VALUE has no slot index, use the previous instruction instead.
      SlotIndex Idx = MBBI == MBB->begin() ?
        LIS->getMBBStartIdx(MBB) :
        LIS->getInstructionIndex(llvm::prior(MBBI)).getRegSlot();
      // Handle consecutive DBG_VALUE instructions with the same slot index.
      do {
        if (handleDebugValue(MBBI, Idx)) {
          MBBI = MBB->erase(MBBI);
          Changed = true;
        } else
          ++MBBI;
      } while (MBBI != MBBE && MBBI->isDebugValue());
    }
  }
  return Changed;
}

void UserValue::extendDef(SlotIndex Idx, unsigned LocNo,
                          LiveInterval *LI, const VNInfo *VNI,
                          SmallVectorImpl<SlotIndex> *Kills,
                          LiveIntervals &LIS, MachineDominatorTree &MDT,
                          UserValueScopes &UVS) {
  SmallVector<SlotIndex, 16> Todo;
  Todo.push_back(Idx);
  do {
    SlotIndex Start = Todo.pop_back_val();
    MachineBasicBlock *MBB = LIS.getMBBFromIndex(Start);
    SlotIndex Stop = LIS.getMBBEndIdx(MBB);
    LocMap::iterator I = locInts.find(Start);

    // Limit to VNI's live range.
    bool ToEnd = true;
    if (LI && VNI) {
      LiveRange *Range = LI->getLiveRangeContaining(Start);
      if (!Range || Range->valno != VNI) {
        if (Kills)
          Kills->push_back(Start);
        continue;
      }
      if (Range->end < Stop)
        Stop = Range->end, ToEnd = false;
    }

    // There could already be a short def at Start.
    if (I.valid() && I.start() <= Start) {
      // Stop when meeting a different location or an already extended interval.
      Start = Start.getNextSlot();
      if (I.value() != LocNo || I.stop() != Start)
        continue;
      // This is a one-slot placeholder. Just skip it.
      ++I;
    }

    // Limited by the next def.
    if (I.valid() && I.start() < Stop)
      Stop = I.start(), ToEnd = false;
    // Limited by VNI's live range.
    else if (!ToEnd && Kills)
      Kills->push_back(Stop);

    if (Start >= Stop)
      continue;

    I.insert(Start, Stop, LocNo);

    // If we extended to the MBB end, propagate down the dominator tree.
    if (!ToEnd)
      continue;
    const std::vector<MachineDomTreeNode*> &Children =
      MDT.getNode(MBB)->getChildren();
    for (unsigned i = 0, e = Children.size(); i != e; ++i) {
      MachineBasicBlock *MBB = Children[i]->getBlock();
      if (UVS.dominates(MBB))
        Todo.push_back(LIS.getMBBStartIdx(MBB));
    }
  } while (!Todo.empty());
}

void
UserValue::addDefsFromCopies(LiveInterval *LI, unsigned LocNo,
                      const SmallVectorImpl<SlotIndex> &Kills,
                      SmallVectorImpl<std::pair<SlotIndex, unsigned> > &NewDefs,
                      MachineRegisterInfo &MRI, LiveIntervals &LIS) {
  if (Kills.empty())
    return;
  // Don't track copies from physregs, there are too many uses.
  if (!TargetRegisterInfo::isVirtualRegister(LI->reg))
    return;

  // Collect all the (vreg, valno) pairs that are copies of LI.
  SmallVector<std::pair<LiveInterval*, const VNInfo*>, 8> CopyValues;
  for (MachineRegisterInfo::use_nodbg_iterator
         UI = MRI.use_nodbg_begin(LI->reg),
         UE = MRI.use_nodbg_end(); UI != UE; ++UI) {
    // Copies of the full value.
    if (UI.getOperand().getSubReg() || !UI->isCopy())
      continue;
    MachineInstr *MI = &*UI;
    unsigned DstReg = MI->getOperand(0).getReg();

    // Don't follow copies to physregs. These are usually setting up call
    // arguments, and the argument registers are always call clobbered. We are
    // better off in the source register which could be a callee-saved register,
    // or it could be spilled.
    if (!TargetRegisterInfo::isVirtualRegister(DstReg))
      continue;

    // Is LocNo extended to reach this copy? If not, another def may be blocking
    // it, or we are looking at a wrong value of LI.
    SlotIndex Idx = LIS.getInstructionIndex(MI);
    LocMap::iterator I = locInts.find(Idx.getRegSlot(true));
    if (!I.valid() || I.value() != LocNo)
      continue;

    if (!LIS.hasInterval(DstReg))
      continue;
    LiveInterval *DstLI = &LIS.getInterval(DstReg);
    const VNInfo *DstVNI = DstLI->getVNInfoAt(Idx.getRegSlot());
    assert(DstVNI && DstVNI->def == Idx.getRegSlot() && "Bad copy value");
    CopyValues.push_back(std::make_pair(DstLI, DstVNI));
  }

  if (CopyValues.empty())
    return;

  DEBUG(dbgs() << "Got " << CopyValues.size() << " copies of " << *LI << '\n');

  // Try to add defs of the copied values for each kill point.
  for (unsigned i = 0, e = Kills.size(); i != e; ++i) {
    SlotIndex Idx = Kills[i];
    for (unsigned j = 0, e = CopyValues.size(); j != e; ++j) {
      LiveInterval *DstLI = CopyValues[j].first;
      const VNInfo *DstVNI = CopyValues[j].second;
      if (DstLI->getVNInfoAt(Idx) != DstVNI)
        continue;
      // Check that there isn't already a def at Idx
      LocMap::iterator I = locInts.find(Idx);
      if (I.valid() && I.start() <= Idx)
        continue;
      DEBUG(dbgs() << "Kill at " << Idx << " covered by valno #"
                   << DstVNI->id << " in " << *DstLI << '\n');
      MachineInstr *CopyMI = LIS.getInstructionFromIndex(DstVNI->def);
      assert(CopyMI && CopyMI->isCopy() && "Bad copy value");
      unsigned LocNo = getLocationNo(CopyMI->getOperand(0));
      I.insert(Idx, Idx.getNextSlot(), LocNo);
      NewDefs.push_back(std::make_pair(Idx, LocNo));
      break;
    }
  }
}

void
UserValue::computeIntervals(MachineRegisterInfo &MRI,
                            const TargetRegisterInfo &TRI,
                            LiveIntervals &LIS,
                            MachineDominatorTree &MDT,
                            UserValueScopes &UVS) {
  SmallVector<std::pair<SlotIndex, unsigned>, 16> Defs;

  // Collect all defs to be extended (Skipping undefs).
  for (LocMap::const_iterator I = locInts.begin(); I.valid(); ++I)
    if (I.value() != ~0u)
      Defs.push_back(std::make_pair(I.start(), I.value()));

  // Extend all defs, and possibly add new ones along the way.
  for (unsigned i = 0; i != Defs.size(); ++i) {
    SlotIndex Idx = Defs[i].first;
    unsigned LocNo = Defs[i].second;
    const MachineOperand &Loc = locations[LocNo];

    if (!Loc.isReg()) {
      extendDef(Idx, LocNo, 0, 0, 0, LIS, MDT, UVS);
      continue;
    }

    // Register locations are constrained to where the register value is live.
    if (TargetRegisterInfo::isVirtualRegister(Loc.getReg())) {
      LiveInterval *LI = 0;
      const VNInfo *VNI = 0;
      if (LIS.hasInterval(Loc.getReg())) {
        LI = &LIS.getInterval(Loc.getReg());
        VNI = LI->getVNInfoAt(Idx);
      }
      SmallVector<SlotIndex, 16> Kills;
      extendDef(Idx, LocNo, LI, VNI, &Kills, LIS, MDT, UVS);
      if (LI)
        addDefsFromCopies(LI, LocNo, Kills, Defs, MRI, LIS);
      continue;
    }

    // For physregs, use the live range of the first regunit as a guide.
    unsigned Unit = *MCRegUnitIterator(Loc.getReg(), &TRI);
    LiveInterval *LI = &LIS.getRegUnit(Unit);
    const VNInfo *VNI = LI->getVNInfoAt(Idx);
    // Don't track copies from physregs, it is too expensive.
    extendDef(Idx, LocNo, LI, VNI, 0, LIS, MDT, UVS);
  }

  // Finally, erase all the undefs.
  for (LocMap::iterator I = locInts.begin(); I.valid();)
    if (I.value() == ~0u)
      I.erase();
    else
      ++I;
}

void LDVImpl::computeIntervals() {
  for (unsigned i = 0, e = userValues.size(); i != e; ++i) {
    UserValueScopes UVS(userValues[i]->getDebugLoc(), LS);
    userValues[i]->computeIntervals(MF->getRegInfo(), *TRI, *LIS, *MDT, UVS);
    userValues[i]->mapVirtRegs(this);
  }
}

bool LDVImpl::runOnMachineFunction(MachineFunction &mf) {
  MF = &mf;
  LIS = &pass.getAnalysis<LiveIntervals>();
  MDT = &pass.getAnalysis<MachineDominatorTree>();
  TRI = mf.getTarget().getRegisterInfo();
  clear();
  LS.initialize(mf);
  DEBUG(dbgs() << "********** COMPUTING LIVE DEBUG VARIABLES: "
               << mf.getName() << " **********\n");

  bool Changed = collectDebugValues(mf);
  computeIntervals();
  DEBUG(print(dbgs()));
  LS.releaseMemory();
  return Changed;
}

bool LiveDebugVariables::runOnMachineFunction(MachineFunction &mf) {
  if (!EnableLDV)
    return false;
  if (!pImpl)
    pImpl = new LDVImpl(this);
  return static_cast<LDVImpl*>(pImpl)->runOnMachineFunction(mf);
}

void LiveDebugVariables::releaseMemory() {
  if (pImpl)
    static_cast<LDVImpl*>(pImpl)->clear();
}

LiveDebugVariables::~LiveDebugVariables() {
  if (pImpl)
    delete static_cast<LDVImpl*>(pImpl);
}

void UserValue::
renameRegister(unsigned OldReg, unsigned NewReg, unsigned SubIdx,
               const TargetRegisterInfo *TRI) {
  for (unsigned i = locations.size(); i; --i) {
    unsigned LocNo = i - 1;
    MachineOperand &Loc = locations[LocNo];
    if (!Loc.isReg() || Loc.getReg() != OldReg)
      continue;
    if (TargetRegisterInfo::isPhysicalRegister(NewReg))
      Loc.substPhysReg(NewReg, *TRI);
    else
      Loc.substVirtReg(NewReg, SubIdx, *TRI);
    coalesceLocation(LocNo);
  }
}

void LDVImpl::
renameRegister(unsigned OldReg, unsigned NewReg, unsigned SubIdx) {
  UserValue *UV = lookupVirtReg(OldReg);
  if (!UV)
    return;

  if (TargetRegisterInfo::isVirtualRegister(NewReg))
    mapVirtReg(NewReg, UV);
  if (OldReg != NewReg)
    virtRegToEqClass.erase(OldReg);

  do {
    UV->renameRegister(OldReg, NewReg, SubIdx, TRI);
    UV = UV->getNext();
  } while (UV);
}

void LiveDebugVariables::
renameRegister(unsigned OldReg, unsigned NewReg, unsigned SubIdx) {
  if (pImpl)
    static_cast<LDVImpl*>(pImpl)->renameRegister(OldReg, NewReg, SubIdx);
}

//===----------------------------------------------------------------------===//
//                           Live Range Splitting
//===----------------------------------------------------------------------===//

bool
UserValue::splitLocation(unsigned OldLocNo, ArrayRef<LiveInterval*> NewRegs) {
  DEBUG({
    dbgs() << "Splitting Loc" << OldLocNo << '\t';
    print(dbgs(), 0);
  });
  bool DidChange = false;
  LocMap::iterator LocMapI;
  LocMapI.setMap(locInts);
  for (unsigned i = 0; i != NewRegs.size(); ++i) {
    LiveInterval *LI = NewRegs[i];
    if (LI->empty())
      continue;

    // Don't allocate the new LocNo until it is needed.
    unsigned NewLocNo = ~0u;

    // Iterate over the overlaps between locInts and LI.
    LocMapI.find(LI->beginIndex());
    if (!LocMapI.valid())
      continue;
    LiveInterval::iterator LII = LI->advanceTo(LI->begin(), LocMapI.start());
    LiveInterval::iterator LIE = LI->end();
    while (LocMapI.valid() && LII != LIE) {
      // At this point, we know that LocMapI.stop() > LII->start.
      LII = LI->advanceTo(LII, LocMapI.start());
      if (LII == LIE)
        break;

      // Now LII->end > LocMapI.start(). Do we have an overlap?
      if (LocMapI.value() == OldLocNo && LII->start < LocMapI.stop()) {
        // Overlapping correct location. Allocate NewLocNo now.
        if (NewLocNo == ~0u) {
          MachineOperand MO = MachineOperand::CreateReg(LI->reg, false);
          MO.setSubReg(locations[OldLocNo].getSubReg());
          NewLocNo = getLocationNo(MO);
          DidChange = true;
        }

        SlotIndex LStart = LocMapI.start();
        SlotIndex LStop  = LocMapI.stop();

        // Trim LocMapI down to the LII overlap.
        if (LStart < LII->start)
          LocMapI.setStartUnchecked(LII->start);
        if (LStop > LII->end)
          LocMapI.setStopUnchecked(LII->end);

        // Change the value in the overlap. This may trigger coalescing.
        LocMapI.setValue(NewLocNo);

        // Re-insert any removed OldLocNo ranges.
        if (LStart < LocMapI.start()) {
          LocMapI.insert(LStart, LocMapI.start(), OldLocNo);
          ++LocMapI;
          assert(LocMapI.valid() && "Unexpected coalescing");
        }
        if (LStop > LocMapI.stop()) {
          ++LocMapI;
          LocMapI.insert(LII->end, LStop, OldLocNo);
          --LocMapI;
        }
      }

      // Advance to the next overlap.
      if (LII->end < LocMapI.stop()) {
        if (++LII == LIE)
          break;
        LocMapI.advanceTo(LII->start);
      } else {
        ++LocMapI;
        if (!LocMapI.valid())
          break;
        LII = LI->advanceTo(LII, LocMapI.start());
      }
    }
  }

  // Finally, remove any remaining OldLocNo intervals and OldLocNo itself.
  locations.erase(locations.begin() + OldLocNo);
  LocMapI.goToBegin();
  while (LocMapI.valid()) {
    unsigned v = LocMapI.value();
    if (v == OldLocNo) {
      DEBUG(dbgs() << "Erasing [" << LocMapI.start() << ';'
                   << LocMapI.stop() << ")\n");
      LocMapI.erase();
    } else {
      if (v > OldLocNo)
        LocMapI.setValueUnchecked(v-1);
      ++LocMapI;
    }
  }

  DEBUG({dbgs() << "Split result: \t"; print(dbgs(), 0);});
  return DidChange;
}

bool
UserValue::splitRegister(unsigned OldReg, ArrayRef<LiveInterval*> NewRegs) {
  bool DidChange = false;
  // Split locations referring to OldReg. Iterate backwards so splitLocation can
  // safely erase unused locations.
  for (unsigned i = locations.size(); i ; --i) {
    unsigned LocNo = i-1;
    const MachineOperand *Loc = &locations[LocNo];
    if (!Loc->isReg() || Loc->getReg() != OldReg)
      continue;
    DidChange |= splitLocation(LocNo, NewRegs);
  }
  return DidChange;
}

void LDVImpl::splitRegister(unsigned OldReg, ArrayRef<LiveInterval*> NewRegs) {
  bool DidChange = false;
  for (UserValue *UV = lookupVirtReg(OldReg); UV; UV = UV->getNext())
    DidChange |= UV->splitRegister(OldReg, NewRegs);

  if (!DidChange)
    return;

  // Map all of the new virtual registers.
  UserValue *UV = lookupVirtReg(OldReg);
  for (unsigned i = 0; i != NewRegs.size(); ++i)
    mapVirtReg(NewRegs[i]->reg, UV);
}

void LiveDebugVariables::
splitRegister(unsigned OldReg, ArrayRef<LiveInterval*> NewRegs) {
  if (pImpl)
    static_cast<LDVImpl*>(pImpl)->splitRegister(OldReg, NewRegs);
}

void
UserValue::rewriteLocations(VirtRegMap &VRM, const TargetRegisterInfo &TRI) {
  // Iterate over locations in reverse makes it easier to handle coalescing.
  for (unsigned i = locations.size(); i ; --i) {
    unsigned LocNo = i-1;
    MachineOperand &Loc = locations[LocNo];
    // Only virtual registers are rewritten.
    if (!Loc.isReg() || !Loc.getReg() ||
        !TargetRegisterInfo::isVirtualRegister(Loc.getReg()))
      continue;
    unsigned VirtReg = Loc.getReg();
    if (VRM.isAssignedReg(VirtReg) &&
        TargetRegisterInfo::isPhysicalRegister(VRM.getPhys(VirtReg))) {
      // This can create a %noreg operand in rare cases when the sub-register
      // index is no longer available. That means the user value is in a
      // non-existent sub-register, and %noreg is exactly what we want.
      Loc.substPhysReg(VRM.getPhys(VirtReg), TRI);
    } else if (VRM.getStackSlot(VirtReg) != VirtRegMap::NO_STACK_SLOT) {
      // FIXME: Translate SubIdx to a stackslot offset.
      Loc = MachineOperand::CreateFI(VRM.getStackSlot(VirtReg));
    } else {
      Loc.setReg(0);
      Loc.setSubReg(0);
    }
    coalesceLocation(LocNo);
  }
}

/// findInsertLocation - Find an iterator for inserting a DBG_VALUE
/// instruction.
static MachineBasicBlock::iterator
findInsertLocation(MachineBasicBlock *MBB, SlotIndex Idx,
                   LiveIntervals &LIS) {
  SlotIndex Start = LIS.getMBBStartIdx(MBB);
  Idx = Idx.getBaseIndex();

  // Try to find an insert location by going backwards from Idx.
  MachineInstr *MI;
  while (!(MI = LIS.getInstructionFromIndex(Idx))) {
    // We've reached the beginning of MBB.
    if (Idx == Start) {
      MachineBasicBlock::iterator I = MBB->SkipPHIsAndLabels(MBB->begin());
      return I;
    }
    Idx = Idx.getPrevIndex();
  }

  // Don't insert anything after the first terminator, though.
  return MI->isTerminator() ? MBB->getFirstTerminator() :
                              llvm::next(MachineBasicBlock::iterator(MI));
}

DebugLoc UserValue::findDebugLoc() {
  DebugLoc D = dl;
  dl = DebugLoc();
  return D;
}
void UserValue::insertDebugValue(MachineBasicBlock *MBB, SlotIndex Idx,
                                 unsigned LocNo,
                                 LiveIntervals &LIS,
                                 const TargetInstrInfo &TII) {
  MachineBasicBlock::iterator I = findInsertLocation(MBB, Idx, LIS);
  MachineOperand &Loc = locations[LocNo];
  ++NumInsertedDebugValues;

  // Frame index locations may require a target callback.
  if (Loc.isFI()) {
    MachineInstr *MI = TII.emitFrameIndexDebugValue(*MBB->getParent(),
                                          Loc.getIndex(), offset, variable, 
                                                    findDebugLoc());
    if (MI) {
      MBB->insert(I, MI);
      return;
    }
  }
  // This is not a frame index, or the target is happy with a standard FI.
  BuildMI(*MBB, I, findDebugLoc(), TII.get(TargetOpcode::DBG_VALUE))
    .addOperand(Loc).addImm(offset).addMetadata(variable);
}

void UserValue::emitDebugValues(VirtRegMap *VRM, LiveIntervals &LIS,
                                const TargetInstrInfo &TII) {
  MachineFunction::iterator MFEnd = VRM->getMachineFunction().end();

  for (LocMap::const_iterator I = locInts.begin(); I.valid();) {
    SlotIndex Start = I.start();
    SlotIndex Stop = I.stop();
    unsigned LocNo = I.value();
    DEBUG(dbgs() << "\t[" << Start << ';' << Stop << "):" << LocNo);
    MachineFunction::iterator MBB = LIS.getMBBFromIndex(Start);
    SlotIndex MBBEnd = LIS.getMBBEndIdx(MBB);

    DEBUG(dbgs() << " BB#" << MBB->getNumber() << '-' << MBBEnd);
    insertDebugValue(MBB, Start, LocNo, LIS, TII);
    // This interval may span multiple basic blocks.
    // Insert a DBG_VALUE into each one.
    while(Stop > MBBEnd) {
      // Move to the next block.
      Start = MBBEnd;
      if (++MBB == MFEnd)
        break;
      MBBEnd = LIS.getMBBEndIdx(MBB);
      DEBUG(dbgs() << " BB#" << MBB->getNumber() << '-' << MBBEnd);
      insertDebugValue(MBB, Start, LocNo, LIS, TII);
    }
    DEBUG(dbgs() << '\n');
    if (MBB == MFEnd)
      break;

    ++I;
  }
}

void LDVImpl::emitDebugValues(VirtRegMap *VRM) {
  DEBUG(dbgs() << "********** EMITTING LIVE DEBUG VARIABLES **********\n");
  const TargetInstrInfo *TII = MF->getTarget().getInstrInfo();
  for (unsigned i = 0, e = userValues.size(); i != e; ++i) {
    DEBUG(userValues[i]->print(dbgs(), &MF->getTarget()));
    userValues[i]->rewriteLocations(*VRM, *TRI);
    userValues[i]->emitDebugValues(VRM, *LIS, *TII);
  }
}

void LiveDebugVariables::emitDebugValues(VirtRegMap *VRM) {
  if (pImpl)
    static_cast<LDVImpl*>(pImpl)->emitDebugValues(VRM);
}


#ifndef NDEBUG
void LiveDebugVariables::dump() {
  if (pImpl)
    static_cast<LDVImpl*>(pImpl)->print(dbgs());
}
#endif


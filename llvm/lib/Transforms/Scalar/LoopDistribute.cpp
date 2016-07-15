//===- LoopDistribute.cpp - Loop Distribution Pass ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Loop Distribution Pass.  Its main focus is to
// distribute loops that cannot be vectorized due to dependence cycles.  It
// tries to isolate the offending dependences into a new loop allowing
// vectorization of the remaining parts.
//
// For dependence analysis, the pass uses the LoopVectorizer's
// LoopAccessAnalysis.  Because this analysis presumes no change in the order of
// memory operations, special care is taken to preserve the lexical order of
// these operations.
//
// Similarly to the Vectorizer, the pass also supports loop versioning to
// run-time disambiguate potentially overlapping arrays.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/LoopVersioning.h"
#include <list>

#define LDIST_NAME "loop-distribute"
#define DEBUG_TYPE LDIST_NAME

using namespace llvm;

static cl::opt<bool>
    LDistVerify("loop-distribute-verify", cl::Hidden,
                cl::desc("Turn on DominatorTree and LoopInfo verification "
                         "after Loop Distribution"),
                cl::init(false));

static cl::opt<bool> DistributeNonIfConvertible(
    "loop-distribute-non-if-convertible", cl::Hidden,
    cl::desc("Whether to distribute into a loop that may not be "
             "if-convertible by the loop vectorizer"),
    cl::init(false));

static cl::opt<unsigned> DistributeSCEVCheckThreshold(
    "loop-distribute-scev-check-threshold", cl::init(8), cl::Hidden,
    cl::desc("The maximum number of SCEV checks allowed for Loop "
             "Distribution"));

static cl::opt<unsigned> PragmaDistributeSCEVCheckThreshold(
    "loop-distribute-scev-check-threshold-with-pragma", cl::init(128),
    cl::Hidden,
    cl::desc(
        "The maximum number of SCEV checks allowed for Loop "
        "Distribution for loop marked with #pragma loop distribute(enable)"));

// Note that the initial value for this depends on whether the pass is invoked
// directly or from the optimization pipeline.
static cl::opt<bool> EnableLoopDistribute(
    "enable-loop-distribute", cl::Hidden,
    cl::desc("Enable the new, experimental LoopDistribution Pass"));

STATISTIC(NumLoopsDistributed, "Number of loops distributed");

namespace {
/// \brief Maintains the set of instructions of the loop for a partition before
/// cloning.  After cloning, it hosts the new loop.
class InstPartition {
  typedef SmallPtrSet<Instruction *, 8> InstructionSet;

public:
  InstPartition(Instruction *I, Loop *L, bool DepCycle = false)
      : DepCycle(DepCycle), OrigLoop(L), ClonedLoop(nullptr) {
    Set.insert(I);
  }

  /// \brief Returns whether this partition contains a dependence cycle.
  bool hasDepCycle() const { return DepCycle; }

  /// \brief Adds an instruction to this partition.
  void add(Instruction *I) { Set.insert(I); }

  /// \brief Collection accessors.
  InstructionSet::iterator begin() { return Set.begin(); }
  InstructionSet::iterator end() { return Set.end(); }
  InstructionSet::const_iterator begin() const { return Set.begin(); }
  InstructionSet::const_iterator end() const { return Set.end(); }
  bool empty() const { return Set.empty(); }

  /// \brief Moves this partition into \p Other.  This partition becomes empty
  /// after this.
  void moveTo(InstPartition &Other) {
    Other.Set.insert(Set.begin(), Set.end());
    Set.clear();
    Other.DepCycle |= DepCycle;
  }

  /// \brief Populates the partition with a transitive closure of all the
  /// instructions that the seeded instructions dependent on.
  void populateUsedSet() {
    // FIXME: We currently don't use control-dependence but simply include all
    // blocks (possibly empty at the end) and let simplifycfg mostly clean this
    // up.
    for (auto *B : OrigLoop->getBlocks())
      Set.insert(B->getTerminator());

    // Follow the use-def chains to form a transitive closure of all the
    // instructions that the originally seeded instructions depend on.
    SmallVector<Instruction *, 8> Worklist(Set.begin(), Set.end());
    while (!Worklist.empty()) {
      Instruction *I = Worklist.pop_back_val();
      // Insert instructions from the loop that we depend on.
      for (Value *V : I->operand_values()) {
        auto *I = dyn_cast<Instruction>(V);
        if (I && OrigLoop->contains(I->getParent()) && Set.insert(I).second)
          Worklist.push_back(I);
      }
    }
  }

  /// \brief Clones the original loop.
  ///
  /// Updates LoopInfo and DominatorTree using the information that block \p
  /// LoopDomBB dominates the loop.
  Loop *cloneLoopWithPreheader(BasicBlock *InsertBefore, BasicBlock *LoopDomBB,
                               unsigned Index, LoopInfo *LI,
                               DominatorTree *DT) {
    ClonedLoop = ::cloneLoopWithPreheader(InsertBefore, LoopDomBB, OrigLoop,
                                          VMap, Twine(".ldist") + Twine(Index),
                                          LI, DT, ClonedLoopBlocks);
    return ClonedLoop;
  }

  /// \brief The cloned loop.  If this partition is mapped to the original loop,
  /// this is null.
  const Loop *getClonedLoop() const { return ClonedLoop; }

  /// \brief Returns the loop where this partition ends up after distribution.
  /// If this partition is mapped to the original loop then use the block from
  /// the loop.
  const Loop *getDistributedLoop() const {
    return ClonedLoop ? ClonedLoop : OrigLoop;
  }

  /// \brief The VMap that is populated by cloning and then used in
  /// remapinstruction to remap the cloned instructions.
  ValueToValueMapTy &getVMap() { return VMap; }

  /// \brief Remaps the cloned instructions using VMap.
  void remapInstructions() {
    remapInstructionsInBlocks(ClonedLoopBlocks, VMap);
  }

  /// \brief Based on the set of instructions selected for this partition,
  /// removes the unnecessary ones.
  void removeUnusedInsts() {
    SmallVector<Instruction *, 8> Unused;

    for (auto *Block : OrigLoop->getBlocks())
      for (auto &Inst : *Block)
        if (!Set.count(&Inst)) {
          Instruction *NewInst = &Inst;
          if (!VMap.empty())
            NewInst = cast<Instruction>(VMap[NewInst]);

          assert(!isa<BranchInst>(NewInst) &&
                 "Branches are marked used early on");
          Unused.push_back(NewInst);
        }

    // Delete the instructions backwards, as it has a reduced likelihood of
    // having to update as many def-use and use-def chains.
    for (auto *Inst : reverse(Unused)) {
      if (!Inst->use_empty())
        Inst->replaceAllUsesWith(UndefValue::get(Inst->getType()));
      Inst->eraseFromParent();
    }
  }

  void print() const {
    if (DepCycle)
      dbgs() << "  (cycle)\n";
    for (auto *I : Set)
      // Prefix with the block name.
      dbgs() << "  " << I->getParent()->getName() << ":" << *I << "\n";
  }

  void printBlocks() const {
    for (auto *BB : getDistributedLoop()->getBlocks())
      dbgs() << *BB;
  }

private:
  /// \brief Instructions from OrigLoop selected for this partition.
  InstructionSet Set;

  /// \brief Whether this partition contains a dependence cycle.
  bool DepCycle;

  /// \brief The original loop.
  Loop *OrigLoop;

  /// \brief The cloned loop.  If this partition is mapped to the original loop,
  /// this is null.
  Loop *ClonedLoop;

  /// \brief The blocks of ClonedLoop including the preheader.  If this
  /// partition is mapped to the original loop, this is empty.
  SmallVector<BasicBlock *, 8> ClonedLoopBlocks;

  /// \brief These gets populated once the set of instructions have been
  /// finalized. If this partition is mapped to the original loop, these are not
  /// set.
  ValueToValueMapTy VMap;
};

/// \brief Holds the set of Partitions.  It populates them, merges them and then
/// clones the loops.
class InstPartitionContainer {
  typedef DenseMap<Instruction *, int> InstToPartitionIdT;

public:
  InstPartitionContainer(Loop *L, LoopInfo *LI, DominatorTree *DT)
      : L(L), LI(LI), DT(DT) {}

  /// \brief Returns the number of partitions.
  unsigned getSize() const { return PartitionContainer.size(); }

  /// \brief Adds \p Inst into the current partition if that is marked to
  /// contain cycles.  Otherwise start a new partition for it.
  void addToCyclicPartition(Instruction *Inst) {
    // If the current partition is non-cyclic.  Start a new one.
    if (PartitionContainer.empty() || !PartitionContainer.back().hasDepCycle())
      PartitionContainer.emplace_back(Inst, L, /*DepCycle=*/true);
    else
      PartitionContainer.back().add(Inst);
  }

  /// \brief Adds \p Inst into a partition that is not marked to contain
  /// dependence cycles.
  ///
  //  Initially we isolate memory instructions into as many partitions as
  //  possible, then later we may merge them back together.
  void addToNewNonCyclicPartition(Instruction *Inst) {
    PartitionContainer.emplace_back(Inst, L);
  }

  /// \brief Merges adjacent non-cyclic partitions.
  ///
  /// The idea is that we currently only want to isolate the non-vectorizable
  /// partition.  We could later allow more distribution among these partition
  /// too.
  void mergeAdjacentNonCyclic() {
    mergeAdjacentPartitionsIf(
        [](const InstPartition *P) { return !P->hasDepCycle(); });
  }

  /// \brief If a partition contains only conditional stores, we won't vectorize
  /// it.  Try to merge it with a previous cyclic partition.
  void mergeNonIfConvertible() {
    mergeAdjacentPartitionsIf([&](const InstPartition *Partition) {
      if (Partition->hasDepCycle())
        return true;

      // Now, check if all stores are conditional in this partition.
      bool seenStore = false;

      for (auto *Inst : *Partition)
        if (isa<StoreInst>(Inst)) {
          seenStore = true;
          if (!LoopAccessInfo::blockNeedsPredication(Inst->getParent(), L, DT))
            return false;
        }
      return seenStore;
    });
  }

  /// \brief Merges the partitions according to various heuristics.
  void mergeBeforePopulating() {
    mergeAdjacentNonCyclic();
    if (!DistributeNonIfConvertible)
      mergeNonIfConvertible();
  }

  /// \brief Merges partitions in order to ensure that no loads are duplicated.
  ///
  /// We can't duplicate loads because that could potentially reorder them.
  /// LoopAccessAnalysis provides dependency information with the context that
  /// the order of memory operation is preserved.
  ///
  /// Return if any partitions were merged.
  bool mergeToAvoidDuplicatedLoads() {
    typedef DenseMap<Instruction *, InstPartition *> LoadToPartitionT;
    typedef EquivalenceClasses<InstPartition *> ToBeMergedT;

    LoadToPartitionT LoadToPartition;
    ToBeMergedT ToBeMerged;

    // Step through the partitions and create equivalence between partitions
    // that contain the same load.  Also put partitions in between them in the
    // same equivalence class to avoid reordering of memory operations.
    for (PartitionContainerT::iterator I = PartitionContainer.begin(),
                                       E = PartitionContainer.end();
         I != E; ++I) {
      auto *PartI = &*I;

      // If a load occurs in two partitions PartI and PartJ, merge all
      // partitions (PartI, PartJ] into PartI.
      for (Instruction *Inst : *PartI)
        if (isa<LoadInst>(Inst)) {
          bool NewElt;
          LoadToPartitionT::iterator LoadToPart;

          std::tie(LoadToPart, NewElt) =
              LoadToPartition.insert(std::make_pair(Inst, PartI));
          if (!NewElt) {
            DEBUG(dbgs() << "Merging partitions due to this load in multiple "
                         << "partitions: " << PartI << ", "
                         << LoadToPart->second << "\n" << *Inst << "\n");

            auto PartJ = I;
            do {
              --PartJ;
              ToBeMerged.unionSets(PartI, &*PartJ);
            } while (&*PartJ != LoadToPart->second);
          }
        }
    }
    if (ToBeMerged.empty())
      return false;

    // Merge the member of an equivalence class into its class leader.  This
    // makes the members empty.
    for (ToBeMergedT::iterator I = ToBeMerged.begin(), E = ToBeMerged.end();
         I != E; ++I) {
      if (!I->isLeader())
        continue;

      auto PartI = I->getData();
      for (auto PartJ : make_range(std::next(ToBeMerged.member_begin(I)),
                                   ToBeMerged.member_end())) {
        PartJ->moveTo(*PartI);
      }
    }

    // Remove the empty partitions.
    PartitionContainer.remove_if(
        [](const InstPartition &P) { return P.empty(); });

    return true;
  }

  /// \brief Sets up the mapping between instructions to partitions.  If the
  /// instruction is duplicated across multiple partitions, set the entry to -1.
  void setupPartitionIdOnInstructions() {
    int PartitionID = 0;
    for (const auto &Partition : PartitionContainer) {
      for (Instruction *Inst : Partition) {
        bool NewElt;
        InstToPartitionIdT::iterator Iter;

        std::tie(Iter, NewElt) =
            InstToPartitionId.insert(std::make_pair(Inst, PartitionID));
        if (!NewElt)
          Iter->second = -1;
      }
      ++PartitionID;
    }
  }

  /// \brief Populates the partition with everything that the seeding
  /// instructions require.
  void populateUsedSet() {
    for (auto &P : PartitionContainer)
      P.populateUsedSet();
  }

  /// \brief This performs the main chunk of the work of cloning the loops for
  /// the partitions.
  void cloneLoops() {
    BasicBlock *OrigPH = L->getLoopPreheader();
    // At this point the predecessor of the preheader is either the memcheck
    // block or the top part of the original preheader.
    BasicBlock *Pred = OrigPH->getSinglePredecessor();
    assert(Pred && "Preheader does not have a single predecessor");
    BasicBlock *ExitBlock = L->getExitBlock();
    assert(ExitBlock && "No single exit block");
    Loop *NewLoop;

    assert(!PartitionContainer.empty() && "at least two partitions expected");
    // We're cloning the preheader along with the loop so we already made sure
    // it was empty.
    assert(&*OrigPH->begin() == OrigPH->getTerminator() &&
           "preheader not empty");

    // Create a loop for each partition except the last.  Clone the original
    // loop before PH along with adding a preheader for the cloned loop.  Then
    // update PH to point to the newly added preheader.
    BasicBlock *TopPH = OrigPH;
    unsigned Index = getSize() - 1;
    for (auto I = std::next(PartitionContainer.rbegin()),
              E = PartitionContainer.rend();
         I != E; ++I, --Index, TopPH = NewLoop->getLoopPreheader()) {
      auto *Part = &*I;

      NewLoop = Part->cloneLoopWithPreheader(TopPH, Pred, Index, LI, DT);

      Part->getVMap()[ExitBlock] = TopPH;
      Part->remapInstructions();
    }
    Pred->getTerminator()->replaceUsesOfWith(OrigPH, TopPH);

    // Now go in forward order and update the immediate dominator for the
    // preheaders with the exiting block of the previous loop.  Dominance
    // within the loop is updated in cloneLoopWithPreheader.
    for (auto Curr = PartitionContainer.cbegin(),
              Next = std::next(PartitionContainer.cbegin()),
              E = PartitionContainer.cend();
         Next != E; ++Curr, ++Next)
      DT->changeImmediateDominator(
          Next->getDistributedLoop()->getLoopPreheader(),
          Curr->getDistributedLoop()->getExitingBlock());
  }

  /// \brief Removes the dead instructions from the cloned loops.
  void removeUnusedInsts() {
    for (auto &Partition : PartitionContainer)
      Partition.removeUnusedInsts();
  }

  /// \brief For each memory pointer, it computes the partitionId the pointer is
  /// used in.
  ///
  /// This returns an array of int where the I-th entry corresponds to I-th
  /// entry in LAI.getRuntimePointerCheck().  If the pointer is used in multiple
  /// partitions its entry is set to -1.
  SmallVector<int, 8>
  computePartitionSetForPointers(const LoopAccessInfo &LAI) {
    const RuntimePointerChecking *RtPtrCheck = LAI.getRuntimePointerChecking();

    unsigned N = RtPtrCheck->Pointers.size();
    SmallVector<int, 8> PtrToPartitions(N);
    for (unsigned I = 0; I < N; ++I) {
      Value *Ptr = RtPtrCheck->Pointers[I].PointerValue;
      auto Instructions =
          LAI.getInstructionsForAccess(Ptr, RtPtrCheck->Pointers[I].IsWritePtr);

      int &Partition = PtrToPartitions[I];
      // First set it to uninitialized.
      Partition = -2;
      for (Instruction *Inst : Instructions) {
        // Note that this could be -1 if Inst is duplicated across multiple
        // partitions.
        int ThisPartition = this->InstToPartitionId[Inst];
        if (Partition == -2)
          Partition = ThisPartition;
        // -1 means belonging to multiple partitions.
        else if (Partition == -1)
          break;
        else if (Partition != (int)ThisPartition)
          Partition = -1;
      }
      assert(Partition != -2 && "Pointer not belonging to any partition");
    }

    return PtrToPartitions;
  }

  void print(raw_ostream &OS) const {
    unsigned Index = 0;
    for (const auto &P : PartitionContainer) {
      OS << "Partition " << Index++ << " (" << &P << "):\n";
      P.print();
    }
  }

  void dump() const { print(dbgs()); }

#ifndef NDEBUG
  friend raw_ostream &operator<<(raw_ostream &OS,
                                 const InstPartitionContainer &Partitions) {
    Partitions.print(OS);
    return OS;
  }
#endif

  void printBlocks() const {
    unsigned Index = 0;
    for (const auto &P : PartitionContainer) {
      dbgs() << "\nPartition " << Index++ << " (" << &P << "):\n";
      P.printBlocks();
    }
  }

private:
  typedef std::list<InstPartition> PartitionContainerT;

  /// \brief List of partitions.
  PartitionContainerT PartitionContainer;

  /// \brief Mapping from Instruction to partition Id.  If the instruction
  /// belongs to multiple partitions the entry contains -1.
  InstToPartitionIdT InstToPartitionId;

  Loop *L;
  LoopInfo *LI;
  DominatorTree *DT;

  /// \brief The control structure to merge adjacent partitions if both satisfy
  /// the \p Predicate.
  template <class UnaryPredicate>
  void mergeAdjacentPartitionsIf(UnaryPredicate Predicate) {
    InstPartition *PrevMatch = nullptr;
    for (auto I = PartitionContainer.begin(); I != PartitionContainer.end();) {
      auto DoesMatch = Predicate(&*I);
      if (PrevMatch == nullptr && DoesMatch) {
        PrevMatch = &*I;
        ++I;
      } else if (PrevMatch != nullptr && DoesMatch) {
        I->moveTo(*PrevMatch);
        I = PartitionContainer.erase(I);
      } else {
        PrevMatch = nullptr;
        ++I;
      }
    }
  }
};

/// \brief For each memory instruction, this class maintains difference of the
/// number of unsafe dependences that start out from this instruction minus
/// those that end here.
///
/// By traversing the memory instructions in program order and accumulating this
/// number, we know whether any unsafe dependence crosses over a program point.
class MemoryInstructionDependences {
  typedef MemoryDepChecker::Dependence Dependence;

public:
  struct Entry {
    Instruction *Inst;
    unsigned NumUnsafeDependencesStartOrEnd;

    Entry(Instruction *Inst) : Inst(Inst), NumUnsafeDependencesStartOrEnd(0) {}
  };

  typedef SmallVector<Entry, 8> AccessesType;

  AccessesType::const_iterator begin() const { return Accesses.begin(); }
  AccessesType::const_iterator end() const { return Accesses.end(); }

  MemoryInstructionDependences(
      const SmallVectorImpl<Instruction *> &Instructions,
      const SmallVectorImpl<Dependence> &Dependences) {
    Accesses.append(Instructions.begin(), Instructions.end());

    DEBUG(dbgs() << "Backward dependences:\n");
    for (auto &Dep : Dependences)
      if (Dep.isPossiblyBackward()) {
        // Note that the designations source and destination follow the program
        // order, i.e. source is always first.  (The direction is given by the
        // DepType.)
        ++Accesses[Dep.Source].NumUnsafeDependencesStartOrEnd;
        --Accesses[Dep.Destination].NumUnsafeDependencesStartOrEnd;

        DEBUG(Dep.print(dbgs(), 2, Instructions));
      }
  }

private:
  AccessesType Accesses;
};

/// \brief The actual class performing the per-loop work.
class LoopDistributeForLoop {
public:
  LoopDistributeForLoop(Loop *L, Function *F, LoopInfo *LI, DominatorTree *DT,
                        ScalarEvolution *SE, OptimizationRemarkEmitter *ORE)
      : L(L), F(F), LI(LI), LAI(nullptr), DT(DT), SE(SE), ORE(ORE) {
    setForced();
  }

  /// \brief Try to distribute an inner-most loop.
  bool processLoop(LoopAccessLegacyAnalysis *LAA) {
    assert(L->empty() && "Only process inner loops.");

    DEBUG(dbgs() << "\nLDist: In \"" << L->getHeader()->getParent()->getName()
                 << "\" checking " << *L << "\n");

    BasicBlock *PH = L->getLoopPreheader();
    if (!PH)
      return fail("no preheader");
    if (!L->getExitBlock())
      return fail("multiple exit blocks");

    // LAA will check that we only have a single exiting block.
    LAI = &LAA->getInfo(L);

    // Currently, we only distribute to isolate the part of the loop with
    // dependence cycles to enable partial vectorization.
    if (LAI->canVectorizeMemory())
      return fail("memory operations are safe for vectorization");

    auto *Dependences = LAI->getDepChecker().getDependences();
    if (!Dependences || Dependences->empty())
      return fail("no unsafe dependences to isolate");

    InstPartitionContainer Partitions(L, LI, DT);

    // First, go through each memory operation and assign them to consecutive
    // partitions (the order of partitions follows program order).  Put those
    // with unsafe dependences into "cyclic" partition otherwise put each store
    // in its own "non-cyclic" partition (we'll merge these later).
    //
    // Note that a memory operation (e.g. Load2 below) at a program point that
    // has an unsafe dependence (Store3->Load1) spanning over it must be
    // included in the same cyclic partition as the dependent operations.  This
    // is to preserve the original program order after distribution.  E.g.:
    //
    //                NumUnsafeDependencesStartOrEnd  NumUnsafeDependencesActive
    //  Load1   -.                     1                       0->1
    //  Load2    | /Unsafe/            0                       1
    //  Store3  -'                    -1                       1->0
    //  Load4                          0                       0
    //
    // NumUnsafeDependencesActive > 0 indicates this situation and in this case
    // we just keep assigning to the same cyclic partition until
    // NumUnsafeDependencesActive reaches 0.
    const MemoryDepChecker &DepChecker = LAI->getDepChecker();
    MemoryInstructionDependences MID(DepChecker.getMemoryInstructions(),
                                     *Dependences);

    int NumUnsafeDependencesActive = 0;
    for (auto &InstDep : MID) {
      Instruction *I = InstDep.Inst;
      // We update NumUnsafeDependencesActive post-instruction, catch the
      // start of a dependence directly via NumUnsafeDependencesStartOrEnd.
      if (NumUnsafeDependencesActive ||
          InstDep.NumUnsafeDependencesStartOrEnd > 0)
        Partitions.addToCyclicPartition(I);
      else
        Partitions.addToNewNonCyclicPartition(I);
      NumUnsafeDependencesActive += InstDep.NumUnsafeDependencesStartOrEnd;
      assert(NumUnsafeDependencesActive >= 0 &&
             "Negative number of dependences active");
    }

    // Add partitions for values used outside.  These partitions can be out of
    // order from the original program order.  This is OK because if the
    // partition uses a load we will merge this partition with the original
    // partition of the load that we set up in the previous loop (see
    // mergeToAvoidDuplicatedLoads).
    auto DefsUsedOutside = findDefsUsedOutsideOfLoop(L);
    for (auto *Inst : DefsUsedOutside)
      Partitions.addToNewNonCyclicPartition(Inst);

    DEBUG(dbgs() << "Seeded partitions:\n" << Partitions);
    if (Partitions.getSize() < 2)
      return fail("cannot isolate unsafe dependencies");

    // Run the merge heuristics: Merge non-cyclic adjacent partitions since we
    // should be able to vectorize these together.
    Partitions.mergeBeforePopulating();
    DEBUG(dbgs() << "\nMerged partitions:\n" << Partitions);
    if (Partitions.getSize() < 2)
      return fail("cannot isolate unsafe dependencies");

    // Now, populate the partitions with non-memory operations.
    Partitions.populateUsedSet();
    DEBUG(dbgs() << "\nPopulated partitions:\n" << Partitions);

    // In order to preserve original lexical order for loads, keep them in the
    // partition that we set up in the MemoryInstructionDependences loop.
    if (Partitions.mergeToAvoidDuplicatedLoads()) {
      DEBUG(dbgs() << "\nPartitions merged to ensure unique loads:\n"
                   << Partitions);
      if (Partitions.getSize() < 2)
        return fail("cannot isolate unsafe dependencies");
    }

    // Don't distribute the loop if we need too many SCEV run-time checks.
    const SCEVUnionPredicate &Pred = LAI->getPSE().getUnionPredicate();
    if (Pred.getComplexity() > (IsForced.getValueOr(false)
                                    ? PragmaDistributeSCEVCheckThreshold
                                    : DistributeSCEVCheckThreshold))
      return fail("too many SCEV run-time checks needed.\n");

    DEBUG(dbgs() << "\nDistributing loop: " << *L << "\n");
    // We're done forming the partitions set up the reverse mapping from
    // instructions to partitions.
    Partitions.setupPartitionIdOnInstructions();

    // To keep things simple have an empty preheader before we version or clone
    // the loop.  (Also split if this has no predecessor, i.e. entry, because we
    // rely on PH having a predecessor.)
    if (!PH->getSinglePredecessor() || &*PH->begin() != PH->getTerminator())
      SplitBlock(PH, PH->getTerminator(), DT, LI);

    // If we need run-time checks, version the loop now.
    auto PtrToPartition = Partitions.computePartitionSetForPointers(*LAI);
    const auto *RtPtrChecking = LAI->getRuntimePointerChecking();
    const auto &AllChecks = RtPtrChecking->getChecks();
    auto Checks = includeOnlyCrossPartitionChecks(AllChecks, PtrToPartition,
                                                  RtPtrChecking);

    if (!Pred.isAlwaysTrue() || !Checks.empty()) {
      DEBUG(dbgs() << "\nPointers:\n");
      DEBUG(LAI->getRuntimePointerChecking()->printChecks(dbgs(), Checks));
      LoopVersioning LVer(*LAI, L, LI, DT, SE, false);
      LVer.setAliasChecks(std::move(Checks));
      LVer.setSCEVChecks(LAI->getPSE().getUnionPredicate());
      LVer.versionLoop(DefsUsedOutside);
      LVer.annotateLoopWithNoAlias();
    }

    // Create identical copies of the original loop for each partition and hook
    // them up sequentially.
    Partitions.cloneLoops();

    // Now, we remove the instruction from each loop that don't belong to that
    // partition.
    Partitions.removeUnusedInsts();
    DEBUG(dbgs() << "\nAfter removing unused Instrs:\n");
    DEBUG(Partitions.printBlocks());

    if (LDistVerify) {
      LI->verify();
      DT->verifyDomTree();
    }

    ++NumLoopsDistributed;
    // Report the success.
    emitOptimizationRemark(F->getContext(), LDIST_NAME, *F, L->getStartLoc(),
                           "distributed loop");
    return true;
  }

  /// \brief Provide diagnostics then \return with false.
  bool fail(llvm::StringRef Message) {
    LLVMContext &Ctx = F->getContext();
    bool Forced = isForced().getValueOr(false);

    DEBUG(dbgs() << "Skipping; " << Message << "\n");

    // With Rpass-missed report that distribution failed.
    ORE->emitOptimizationRemarkMissed(
        LDIST_NAME, L,
        "loop not distributed: use -Rpass-analysis=loop-distribute for more "
        "info");

    // With Rpass-analysis report why.  This is on by default if distribution
    // was requested explicitly.
    emitOptimizationRemarkAnalysis(
        Ctx, Forced ? DiagnosticInfoOptimizationRemarkAnalysis::AlwaysPrint
                    : LDIST_NAME,
        *F, L->getStartLoc(), Twine("loop not distributed: ") + Message);

    // Also issue a warning if distribution was requested explicitly but it
    // failed.
    if (Forced)
      Ctx.diagnose(DiagnosticInfoOptimizationFailure(
          *F, L->getStartLoc(), "loop not distributed: failed "
                                "explicitly specified loop distribution"));

    return false;
  }

  /// \brief Return if distribution forced to be enabled/disabled for the loop.
  ///
  /// If the optional has a value, it indicates whether distribution was forced
  /// to be enabled (true) or disabled (false).  If the optional has no value
  /// distribution was not forced either way.
  const Optional<bool> &isForced() const { return IsForced; }

private:
  /// \brief Filter out checks between pointers from the same partition.
  ///
  /// \p PtrToPartition contains the partition number for pointers.  Partition
  /// number -1 means that the pointer is used in multiple partitions.  In this
  /// case we can't safely omit the check.
  SmallVector<RuntimePointerChecking::PointerCheck, 4>
  includeOnlyCrossPartitionChecks(
      const SmallVectorImpl<RuntimePointerChecking::PointerCheck> &AllChecks,
      const SmallVectorImpl<int> &PtrToPartition,
      const RuntimePointerChecking *RtPtrChecking) {
    SmallVector<RuntimePointerChecking::PointerCheck, 4> Checks;

    std::copy_if(AllChecks.begin(), AllChecks.end(), std::back_inserter(Checks),
                 [&](const RuntimePointerChecking::PointerCheck &Check) {
                   for (unsigned PtrIdx1 : Check.first->Members)
                     for (unsigned PtrIdx2 : Check.second->Members)
                       // Only include this check if there is a pair of pointers
                       // that require checking and the pointers fall into
                       // separate partitions.
                       //
                       // (Note that we already know at this point that the two
                       // pointer groups need checking but it doesn't follow
                       // that each pair of pointers within the two groups need
                       // checking as well.
                       //
                       // In other words we don't want to include a check just
                       // because there is a pair of pointers between the two
                       // pointer groups that require checks and a different
                       // pair whose pointers fall into different partitions.)
                       if (RtPtrChecking->needsChecking(PtrIdx1, PtrIdx2) &&
                           !RuntimePointerChecking::arePointersInSamePartition(
                               PtrToPartition, PtrIdx1, PtrIdx2))
                         return true;
                   return false;
                 });

    return Checks;
  }

  /// \brief Check whether the loop metadata is forcing distribution to be
  /// enabled/disabled.
  void setForced() {
    Optional<const MDOperand *> Value =
        findStringMetadataForLoop(L, "llvm.loop.distribute.enable");
    if (!Value)
      return;

    const MDOperand *Op = *Value;
    assert(Op && mdconst::hasa<ConstantInt>(*Op) && "invalid metadata");
    IsForced = mdconst::extract<ConstantInt>(*Op)->getZExtValue();
  }

  Loop *L;
  Function *F;

  // Analyses used.
  LoopInfo *LI;
  const LoopAccessInfo *LAI;
  DominatorTree *DT;
  ScalarEvolution *SE;
  OptimizationRemarkEmitter *ORE;

  /// \brief Indicates whether distribution is forced to be enabled/disabled for
  /// the loop.
  ///
  /// If the optional has a value, it indicates whether distribution was forced
  /// to be enabled (true) or disabled (false).  If the optional has no value
  /// distribution was not forced either way.
  Optional<bool> IsForced;
};

/// \brief The pass class.
class LoopDistribute : public FunctionPass {
public:
  /// \p ProcessAllLoopsByDefault specifies whether loop distribution should be
  /// performed by default.  Pass -enable-loop-distribute={0,1} overrides this
  /// default.  We use this to keep LoopDistribution off by default when invoked
  /// from the optimization pipeline but on when invoked explicitly from opt.
  LoopDistribute(bool ProcessAllLoopsByDefault = true)
      : FunctionPass(ID), ProcessAllLoops(ProcessAllLoopsByDefault) {
    // The default is set by the caller.
    if (EnableLoopDistribute.getNumOccurrences() > 0)
      ProcessAllLoops = EnableLoopDistribute;
    initializeLoopDistributePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;

    auto *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto *LAA = &getAnalysis<LoopAccessLegacyAnalysis>();
    auto *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    auto *ORE = &getAnalysis<OptimizationRemarkEmitter>();

    // Build up a worklist of inner-loops to vectorize. This is necessary as the
    // act of distributing a loop creates new loops and can invalidate iterators
    // across the loops.
    SmallVector<Loop *, 8> Worklist;

    for (Loop *TopLevelLoop : *LI)
      for (Loop *L : depth_first(TopLevelLoop))
        // We only handle inner-most loops.
        if (L->empty())
          Worklist.push_back(L);

    // Now walk the identified inner loops.
    bool Changed = false;
    for (Loop *L : Worklist) {
      LoopDistributeForLoop LDL(L, &F, LI, DT, SE, ORE);

      // If distribution was forced for the specific loop to be
      // enabled/disabled, follow that.  Otherwise use the global flag.
      if (LDL.isForced().getValueOr(ProcessAllLoops))
        Changed |= LDL.processLoop(LAA);
    }

    // Process each loop nest in the function.
    return Changed;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addRequired<LoopAccessLegacyAnalysis>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addRequired<OptimizationRemarkEmitter>();
  }

  static char ID;

private:
  /// \brief Whether distribution should be on in this function.  The per-loop
  /// pragma can override this.
  bool ProcessAllLoops;
};
} // anonymous namespace

char LoopDistribute::ID;
static const char ldist_name[] = "Loop Distribition";

INITIALIZE_PASS_BEGIN(LoopDistribute, LDIST_NAME, ldist_name, false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopAccessLegacyAnalysis)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(OptimizationRemarkEmitter)
INITIALIZE_PASS_END(LoopDistribute, LDIST_NAME, ldist_name, false, false)

namespace llvm {
FunctionPass *createLoopDistributePass(bool ProcessAllLoopsByDefault) {
  return new LoopDistribute(ProcessAllLoopsByDefault);
}
}

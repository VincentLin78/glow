// Copyright 2017 Facebook Inc.  All Rights Reserved.

#include "glow/Graph/Graph.h"
#include "glow/Graph/Node.h"
#include "glow/Optimizer/Optimizer.h"
#include "glow/Support/Casting.h"

#include <unordered_map>
#include <unordered_set>

using namespace glow;

/// Dead code elimination.
static void DCE(Graph &G) {
  auto &nodes = G.getNodes();
  auto &vars = G.getVars();

  // Remove unused nodes. Do not remove unused vars because they are the
  // interface to the user program.
  bool changedLocally = true;
  do {
    changedLocally = false;
    for (auto it = nodes.begin(), e = nodes.end(); it != e;) {
      bool used = (*it)->hasUsers();
      if (used || isa<SaveNode>(*it)) {
        it++;
        continue;
      }

      delete *it;
      it = nodes.erase(it);
      changedLocally = true;
    }

  } while (changedLocally);

  // Delete unused variables.
  for (auto it = vars.begin(), e = vars.end(); it != e;) {
    if ((*it)->hasUsers()) {
      it++;
      continue;
    }

    delete *it;
    it = vars.erase(it);
  }
}

/// \returns true if the masks \p shuffle1 and shuffle2 are
/// the inverse of on another. Applying both masks should result in the identity
/// shuffle.
static bool isIdentityShuffle(llvm::ArrayRef<unsigned> shuffle1,
                              llvm::ArrayRef<unsigned> shuffle2) {

  if (shuffle1.size() != shuffle2.size()) {
    return false;
  }

  // Check if the combined masks are the identity mask.
  for (unsigned i = 0, e = shuffle1.size(); i < e; i++) {
    unsigned idx = shuffle2[shuffle1[i]];
    if (idx != i) {
      return false;
    }
  }
  return true;
}

/// Dead code elimination.
static void SinkTranspose(Graph &G) {
  auto &nodes = G.getNodes();

  // For each node:
  for (auto it = nodes.begin(), e = nodes.end(); it != e; ++it) {
    // Sink Transpose below batch normalization nodes:
    if (auto *BN = dyn_cast<BatchNormalizationNode>(*it)) {
      auto *TR = dyn_cast<TransposeNode>(BN->getInput());

      if (!TR) {
        continue;
      }

      // Figure out where we transposed the channel index for batch
      // normalization.
      unsigned idx = BN->getChannelIdx();
      unsigned newChannelIdx = TR->getShuffle()[idx];

      auto *NewBN = G.createBatchNormalization(
          BN->getName(), TR->getInput(), BN->getBias(), BN->getScale(),
          BN->getMean(), BN->getVar(), newChannelIdx, BN->getEpsilon(),
          BN->getMomentum());
      auto *newTR = G.createTranspose(TR->getName(), NewBN, TR->getShuffle());

      BN->replaceAllUsesOfWith(newTR);
      continue;
    }

    // Sink Transpose below batch RELU nodes.
    // TODO: support other similar activation functions, such as sigmoid, etc.
    if (auto *RL = dyn_cast<ReluNode>(*it)) {
      auto *TR = dyn_cast<TransposeNode>(RL->getInput());

      if (!TR) {
        continue;
      }

      auto *NRL = G.createRELU(RL->getName(), TR->getInput());
      auto *newTR = G.createTranspose(TR->getName(), NRL, TR->getShuffle());
      RL->replaceAllUsesOfWith(newTR);
      continue;
    }

    // Merge consecutive Transpose operations.
    if (auto *TR1 = dyn_cast<TransposeNode>(*it)) {
      auto *TR2 = dyn_cast<TransposeNode>(TR1->getInput());

      if (!TR2) {
        continue;
      }

      auto mask1 = TR1->getShuffle();
      auto mask2 = TR2->getShuffle();
      assert(mask1.size() == mask2.size() && "Invalid mask size");

      // The two transposes are reversing one another. We can skip both of them
      // alltogether.
      if (isIdentityShuffle(mask1, mask2)) {
        TR1->replaceAllUsesOfWith(TR2->getInput());
        continue;
      }
    }

    // Sink Transpose below batch Arithmetic nodes.
    if (auto *AN = dyn_cast<ArithmeticNode>(*it)) {
      auto *LTR = dyn_cast<TransposeNode>(AN->getLHS());
      auto *RTR = dyn_cast<TransposeNode>(AN->getRHS());

      if (!LTR || !RTR) {
        continue;
      }
      // The masks of the transposes on both sizes must match.
      if (LTR->getShuffle() != RTR->getShuffle()) {
        continue;
      }

      auto *newAN = G.createArithmetic(AN->getName(), LTR->getInput(),
                                       RTR->getInput(), AN->getMode());
      auto *newTR = G.createTranspose(LTR->getName(), newAN, LTR->getShuffle());
      AN->replaceAllUsesOfWith(newTR);
    }

    // Sink Transpose below batch Arithmetic nodes.
    if (auto *CN = dyn_cast<ConcatNode>(*it)) {
      assert(CN->getInputs().size() > 1 && "Invalid number of concat operands");

      // Collect all of the transpose nodes and their inputs.
      std::vector<Node *> inputs;
      std::vector<TransposeNode *> transposes;
      for (auto &in : CN->getInputs()) {

        if (auto *II = dyn_cast<TransposeNode>(in.get())) {
          transposes.push_back(II);
          inputs.push_back(II->getInput());
          continue;
        }

        break;
      }

      // If some of the inputs were not transposes then bail out.
      if (CN->getInputs().size() != transposes.size()) {
        continue;
      }

      auto *first = transposes[0];
      auto firstMask = first->getShuffle();
      bool sameMask = true;
      for (auto *T : transposes) {
        if (T->getShuffle() != firstMask) {
          sameMask = false;
          break;
        }
      }

      // If the shuffle masks don't agree then bail out.
      if (!sameMask) {
        continue;
      }

      // Figure out where we transposed the channel index for batch
      // normalization.
      unsigned idx = CN->getDim();
      unsigned newChannelIdx = firstMask[idx];

      auto *newCN = G.createConcat(CN->getName(), inputs, newChannelIdx);
      auto *newTR = G.createTranspose(first->getName(), newCN, firstMask);
      CN->replaceAllUsesOfWith(newTR);
    }

  } // For all nodes in the graph.
}

/// Dead code elimination.
static void OptimizePool(Graph &G) {
  auto &nodes = G.getNodes();

  // For each node:
  for (auto it = nodes.begin(), e = nodes.end(); it != e; ++it) {
    // Swap the order of Relu->MaxPool, to perform the RELU operation on a
    // smaller tensor. This optimization is not a major performance win. The
    // RELU operation takes a small fraction of the time, and reordering the
    // nodes does not give us much. However, reordering the buffers allows us to
    // reuse the memory buffer of the pool operation and potentially save
    // memory.
    if (auto *PL = dyn_cast<PoolNode>(*it)) {
      auto *RL = dyn_cast<ReluNode>(PL->getInput());

      if (!RL) {
        continue;
      }

      // This optimization is only valid on max pooling.
      if (PL->getMode() != PoolNode::Mode::Max) {
        continue;
      }

      // We don't want to increase the number of operations in the program, so
      // perform this transformation if the relu has a single user, which is the
      // pooling operation.
      if (!RL->hasOneUse()) {
        continue;
      }

      auto *NPL = G.createPool(PL->getName(), RL->getInput(), PL->getMode(),
                               PL->getKernel(), PL->getStride(), PL->getPad());
      auto *NRL = G.createRELU(RL->getName(), NPL);
      PL->replaceAllUsesOfWith(NRL);
      continue;
    }
  } // For all nodes in the graph.
}

static void OptimizeBatchNorm(Graph &G) {
  auto &nodes = G.getNodes();

  // For each node:
  for (auto it = nodes.begin(), e = nodes.end(); it != e; ++it) {
    // Merge the Batch Normalization operation into the convolution that comes
    // before it by updating the weights of the filter.
    if (auto *BN = dyn_cast<BatchNormalizationNode>(*it)) {
      auto *CV = dyn_cast<ConvolutionNode>(BN->getInput());
      if (!CV) {
        continue;
      }

      // We can't modify conv operators that have multiple users.
      if (!CV->hasOneUse()) {
        continue;
      }

      // First, BN computation can be phrased as follows:
      //
      // (X - mean) * (1.0 / sqrt(var + eps)) * bn_scale + bias
      //
      // Thus, we can rewrite bn_scale as:
      //  X * bn_scale * 1.0 / (sqrt(var + eps)) +
      //    (bias - mean * (1.0 / sqrt(var + eps)) * bn_scale)
      //
      // Thus, can just have the affine transform:
      //
      //  X * A + B
      //
      //  where
      //
      //  A = bn_scale * 1.0 / (sqrt(running_var + eps))
      //  B =  (bias - mean * (1.0 / sqrt(var + eps)) * bn_scale)
      //
      // Now, we have that the computation made is the following:
      //
      // ((X `conv` W) + b) * A + B
      //
      // Then, we can simply fuse this as follows:
      //
      // (X `conv` (W * A)) + b * A + B
      //
      // which is simply
      //
      // (X `conv` Q) + C
      //
      // where
      //
      // Q = W * A
      // C = b * A + B

      auto filterH = cast<Variable>(CV->getFilter())->getHandle<FloatTy>();
      auto cbiasH = cast<Variable>(CV->getBias())->getHandle<FloatTy>();

      auto scaleH = cast<Variable>(BN->getScale())->getHandle<FloatTy>();
      auto biasH = cast<Variable>(BN->getBias())->getHandle<FloatTy>();
      auto meanH = cast<Variable>(BN->getMean())->getHandle<FloatTy>();
      auto varH = cast<Variable>(BN->getVar())->getHandle<FloatTy>();

      // Update the filater/bias variables of the Conv node.
      auto epsilon = BN->getEpsilon();
      for (size_t i = 0, e = filterH.size(); i < e; i++) {
        // Dimension zero is the 'channel' dimension. If we ever change the
        // layout of the filter then we need to change this optimization.
        size_t channelId = filterH.getDimForPtr(0, i);
        FloatTy var = varH.at({channelId});
        FloatTy stdvar = FloatTy(1.0) / std::sqrt(var + epsilon);
        FloatTy gamma = scaleH.at({channelId});
        FloatTy A = gamma * stdvar;
        filterH.raw(i) = filterH.raw(i) * A;
      }

      for (size_t i = 0, e = cbiasH.size(); i < e; i++) {
        // Dimension zero is the 'channel' dimension. If we ever change the
        // layout of the filter then we need to change this optimization.
        size_t channelId = cbiasH.getDimForPtr(0, i);
        FloatTy mu = meanH.at({channelId});
        FloatTy var = varH.at({channelId});
        FloatTy stdvar = FloatTy(1.0) / std::sqrt(var + epsilon);
        FloatTy gamma = scaleH.at({channelId});
        FloatTy beta = biasH.at({channelId});
        FloatTy A = gamma * stdvar;
        FloatTy B = beta - mu * A;
        cbiasH.raw(i) = cbiasH.raw(i) * A + B;
      }

      BN->replaceAllUsesOfWith(CV);
    }
  } // For all nodes in the graph.
}

void glow::optimize(Graph &G, OptimizationMode mode) {
  if (mode == OptimizationMode::None) {
    return;
  }

  // Sink transpose operations in an attempt to cancel them out.
  SinkTranspose(G);

  // Optimize the pooling operation.
  OptimizePool(G);

  // Perform Dead Code Elimination.
  DCE(G);

  if (mode == OptimizationMode::Infer) {
    // Merge batch normalization operations.
    OptimizeBatchNorm(G);
  }

  // Perform Dead Code Elimination.
  DCE(G);
}
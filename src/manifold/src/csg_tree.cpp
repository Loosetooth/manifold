// Copyright 2022 The Manifold Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#if MANIFOLD_PAR == 'T' && __has_include(<tbb/tbb.h>)
#include <tbb/tbb.h>
#define TBB_PREVIEW_CONCURRENT_ORDERED_CONTAINERS 1
#include <tbb/concurrent_priority_queue.h>
#endif

#include <algorithm>
#include <variant>

#include "boolean3.h"
#include "csg_tree.h"
#include "impl.h"
#include "mesh_fixes.h"
#include "par.h"

constexpr int kParallelThreshold = 4096;

namespace {
using namespace manifold;
struct Transform4x3 {
  const glm::mat4x3 transform;

  glm::vec3 operator()(glm::vec3 position) {
    return transform * glm::vec4(position, 1.0f);
  }
};

struct UpdateHalfedge {
  const int nextVert;
  const int nextEdge;
  const int nextFace;

  Halfedge operator()(Halfedge edge) {
    edge.startVert += nextVert;
    edge.endVert += nextVert;
    edge.pairedHalfedge += nextEdge;
    edge.face += nextFace;
    return edge;
  }
};

struct UpdateTriProp {
  const int nextProp;

  glm::ivec3 operator()(glm::ivec3 tri) {
    tri += nextProp;
    return tri;
  }
};

struct UpdateMeshIDs {
  const int offset;

  TriRef operator()(TriRef ref) {
    ref.meshID += offset;
    return ref;
  }
};

struct CheckOverlap {
  const Box *boxes;
  const size_t i;
  bool operator()(int j) { return boxes[i].DoesOverlap(boxes[j]); }
};
}  // namespace
namespace manifold {

std::shared_ptr<CsgNode> CsgNode::Boolean(
    const std::shared_ptr<CsgNode> &second, OpType op) {
  if (auto opNode = std::dynamic_pointer_cast<CsgOpNode>(second)) {
    // "this" is not a CsgOpNode (which overrides Boolean), but if "second" is
    // and the operation is commutative, we let it built the tree.
    if ((op == OpType::Add || op == OpType::Intersect)) {
      return opNode->Boolean(shared_from_this(), op);
    }
  }
  std::vector<std::shared_ptr<CsgNode>> children({shared_from_this(), second});
  return std::make_shared<CsgOpNode>(children, op);
}

std::shared_ptr<CsgNode> CsgNode::Translate(const glm::vec3 &t) const {
  glm::mat4x3 transform(1.0f);
  transform[3] += t;
  return Transform(transform);
}

std::shared_ptr<CsgNode> CsgNode::Scale(const glm::vec3 &v) const {
  glm::mat4x3 transform(1.0f);
  for (int i : {0, 1, 2}) transform[i] *= v;
  return Transform(transform);
}

std::shared_ptr<CsgNode> CsgNode::Rotate(float xDegrees, float yDegrees,
                                         float zDegrees) const {
  glm::mat3 rX(1.0f, 0.0f, 0.0f,                      //
               0.0f, cosd(xDegrees), sind(xDegrees),  //
               0.0f, -sind(xDegrees), cosd(xDegrees));
  glm::mat3 rY(cosd(yDegrees), 0.0f, -sind(yDegrees),  //
               0.0f, 1.0f, 0.0f,                       //
               sind(yDegrees), 0.0f, cosd(yDegrees));
  glm::mat3 rZ(cosd(zDegrees), sind(zDegrees), 0.0f,   //
               -sind(zDegrees), cosd(zDegrees), 0.0f,  //
               0.0f, 0.0f, 1.0f);
  glm::mat4x3 transform(rZ * rY * rX);
  return Transform(transform);
}

CsgLeafNode::CsgLeafNode() : pImpl_(std::make_shared<Manifold::Impl>()) {}

CsgLeafNode::CsgLeafNode(std::shared_ptr<const Manifold::Impl> pImpl_)
    : pImpl_(pImpl_) {}

CsgLeafNode::CsgLeafNode(std::shared_ptr<const Manifold::Impl> pImpl_,
                         glm::mat4x3 transform_)
    : pImpl_(pImpl_), transform_(transform_) {}

std::shared_ptr<const Manifold::Impl> CsgLeafNode::GetImpl() const {
  if (transform_ == glm::mat4x3(1.0f)) return pImpl_;
  pImpl_ =
      std::make_shared<const Manifold::Impl>(pImpl_->Transform(transform_));
  transform_ = glm::mat4x3(1.0f);
  return pImpl_;
}

glm::mat4x3 CsgLeafNode::GetTransform() const { return transform_; }

std::shared_ptr<CsgLeafNode> CsgLeafNode::ToLeafNode() const {
  return std::make_shared<CsgLeafNode>(*this);
}

std::shared_ptr<CsgNode> CsgLeafNode::Transform(const glm::mat4x3 &m) const {
  return std::make_shared<CsgLeafNode>(pImpl_, m * glm::mat4(transform_));
}

CsgNodeType CsgLeafNode::GetNodeType() const { return CsgNodeType::Leaf; }

/**
 * Efficient union of a set of pairwise disjoint meshes.
 */
Manifold::Impl CsgLeafNode::Compose(
    const std::vector<std::shared_ptr<CsgLeafNode>> &nodes) {
  float precision = -1;
  int numVert = 0;
  int numEdge = 0;
  int numTri = 0;
  int numPropVert = 0;
  std::vector<int> vertIndices;
  std::vector<int> edgeIndices;
  std::vector<int> triIndices;
  std::vector<int> propVertIndices;
  int numPropOut = 0;
  for (auto &node : nodes) {
    float nodeOldScale = node->pImpl_->bBox_.Scale();
    float nodeNewScale =
        node->pImpl_->bBox_.Transform(node->transform_).Scale();
    float nodePrecision = node->pImpl_->precision_;
    nodePrecision *= glm::max(1.0f, nodeNewScale / nodeOldScale);
    nodePrecision = glm::max(nodePrecision, kTolerance * nodeNewScale);
    if (!glm::isfinite(nodePrecision)) nodePrecision = -1;
    precision = glm::max(precision, nodePrecision);

    vertIndices.push_back(numVert);
    edgeIndices.push_back(numEdge * 2);
    triIndices.push_back(numTri);
    propVertIndices.push_back(numPropVert);
    numVert += node->pImpl_->NumVert();
    numEdge += node->pImpl_->NumEdge();
    numTri += node->pImpl_->NumTri();
    const int numProp = node->pImpl_->NumProp();
    numPropOut = glm::max(numPropOut, numProp);
    numPropVert +=
        numProp == 0 ? 1
                     : node->pImpl_->meshRelation_.properties.size() / numProp;
  }

  Manifold::Impl combined;
  combined.precision_ = precision;
  combined.vertPos_.resize(numVert);
  combined.halfedge_.resize(2 * numEdge);
  combined.faceNormal_.resize(numTri);
  combined.halfedgeTangent_.resize(2 * numEdge);
  combined.meshRelation_.triRef.resize(numTri);
  if (numPropOut > 0) {
    combined.meshRelation_.numProp = numPropOut;
    combined.meshRelation_.properties.resize(numPropOut * numPropVert, 0);
    combined.meshRelation_.triProperties.resize(numTri);
  }
  auto policy = autoPolicy(numTri);

  // if we are already parallelizing for each node, do not perform multithreaded
  // copying as it will slightly hurt performance
  if (nodes.size() > 1 && policy == ExecutionPolicy::Par)
    policy = ExecutionPolicy::Seq;

  for_each_n_host(
      nodes.size() > 1 ? ExecutionPolicy::Par : ExecutionPolicy::Seq,
      countAt(0), nodes.size(),
      [&nodes, &vertIndices, &edgeIndices, &triIndices, &propVertIndices,
       numPropOut, &combined, policy](int i) {
        auto &node = nodes[i];
        copy(policy, node->pImpl_->halfedgeTangent_.begin(),
             node->pImpl_->halfedgeTangent_.end(),
             combined.halfedgeTangent_.begin() + edgeIndices[i]);
        transform(
            policy, node->pImpl_->halfedge_.begin(),
            node->pImpl_->halfedge_.end(),
            combined.halfedge_.begin() + edgeIndices[i],
            UpdateHalfedge({vertIndices[i], edgeIndices[i], triIndices[i]}));

        if (numPropOut > 0) {
          auto start =
              combined.meshRelation_.triProperties.begin() + triIndices[i];
          if (node->pImpl_->NumProp() > 0) {
            auto &triProp = node->pImpl_->meshRelation_.triProperties;
            transform(policy, triProp.begin(), triProp.end(), start,
                      UpdateTriProp({propVertIndices[i]}));

            const int numProp = node->pImpl_->NumProp();
            auto &oldProp = node->pImpl_->meshRelation_.properties;
            auto &newProp = combined.meshRelation_.properties;
            for (int p = 0; p < numProp; ++p) {
              strided_range<VecDH<float>::IterC> oldRange(
                  oldProp.begin() + p, oldProp.end(), numProp);
              strided_range<VecDH<float>::Iter> newRange(
                  newProp.begin() + numPropOut * propVertIndices[i] + p,
                  newProp.end(), numPropOut);
              copy(policy, oldRange.begin(), oldRange.end(), newRange.begin());
            }
          } else {
            // point all triangles at single new property of zeros.
            fill(policy, start, start + node->pImpl_->NumTri(),
                 glm::ivec3(propVertIndices[i]));
          }
        }

        if (node->transform_ == glm::mat4x3(1.0f)) {
          copy(policy, node->pImpl_->vertPos_.begin(),
               node->pImpl_->vertPos_.end(),
               combined.vertPos_.begin() + vertIndices[i]);
          copy(policy, node->pImpl_->faceNormal_.begin(),
               node->pImpl_->faceNormal_.end(),
               combined.faceNormal_.begin() + triIndices[i]);
        } else {
          // no need to apply the transform to the node, just copy the vertices
          // and face normals and apply transform on the fly
          auto vertPosBegin = thrust::make_transform_iterator(
              node->pImpl_->vertPos_.begin(), Transform4x3({node->transform_}));
          glm::mat3 normalTransform =
              glm::inverse(glm::transpose(glm::mat3(node->transform_)));
          auto faceNormalBegin = thrust::make_transform_iterator(
              node->pImpl_->faceNormal_.begin(),
              TransformNormals({normalTransform}));
          copy_n(policy, vertPosBegin, node->pImpl_->vertPos_.size(),
                 combined.vertPos_.begin() + vertIndices[i]);
          copy_n(policy, faceNormalBegin, node->pImpl_->faceNormal_.size(),
                 combined.faceNormal_.begin() + triIndices[i]);

          const bool invert = glm::determinant(glm::mat3(node->transform_)) < 0;
          for_each_n(policy,
                     zip(combined.halfedgeTangent_.begin() + edgeIndices[i],
                         countAt(0)),
                     node->pImpl_->halfedgeTangent_.size(),
                     TransformTangents{glm::mat3(node->transform_), invert,
                                       node->pImpl_->halfedgeTangent_.cptrD(),
                                       node->pImpl_->halfedge_.cptrD()});
          if (invert)
            for_each_n(policy,
                       zip(combined.meshRelation_.triRef.begin(),
                           countAt(triIndices[i])),
                       node->pImpl_->NumTri(),
                       FlipTris({combined.halfedge_.ptrD()}));
        }
        // Since the nodes may be copies containing the same meshIDs, it is
        // important to add an offset so that each node instance gets
        // unique meshIDs.
        const int offset = i * Manifold::Impl::meshIDCounter_;
        transform(policy, node->pImpl_->meshRelation_.triRef.begin(),
                  node->pImpl_->meshRelation_.triRef.end(),
                  combined.meshRelation_.triRef.begin() + triIndices[i],
                  UpdateMeshIDs({offset}));
      });

  for (int i = 0; i < nodes.size(); i++) {
    auto &node = nodes[i];
    const int offset = i * Manifold::Impl::meshIDCounter_;

    for (const auto pair : node->pImpl_->meshRelation_.meshIDtransform) {
      combined.meshRelation_.meshIDtransform[pair.first + offset] = pair.second;
    }
  }

  // required to remove parts that are smaller than the precision
  combined.SimplifyTopology();
  combined.Finish();
  combined.IncrementMeshIDs();
  return combined;
}

CsgOpNode::CsgOpNode() {}

CsgOpNode::CsgOpNode(const std::vector<std::shared_ptr<CsgNode>> &children,
                     OpType op)
    : impl_(Impl{}) {
  auto impl = impl_.GetGuard();
  impl->children_ = children;
  SetOp(op);
}

CsgOpNode::CsgOpNode(std::vector<std::shared_ptr<CsgNode>> &&children,
                     OpType op)
    : impl_(Impl{}) {
  auto impl = impl_.GetGuard();
  impl->children_ = children;
  SetOp(op);
}

std::shared_ptr<CsgNode> CsgOpNode::Boolean(
    const std::shared_ptr<CsgNode> &second, OpType op) {
  std::vector<std::shared_ptr<CsgNode>> children;

  auto isReused = [](const auto &node) { return node->impl_.UseCount() > 1; };

  auto copyChildren = [&](const auto &list, const glm::mat4x3 &transform) {
    for (const auto &child : list) {
      children.push_back(child->Transform(transform));
    }
  };

  auto self = std::dynamic_pointer_cast<CsgOpNode>(shared_from_this());
  assert(self);
  if (IsOp(op) && !isReused(self)) {
    auto impl = impl_.GetGuard();
    copyChildren(impl->children_, transform_);
  } else {
    children.push_back(self);
  }

  auto secondOp = std::dynamic_pointer_cast<CsgOpNode>(second);
  auto canInlineSecondOp = [&]() {
    switch (op) {
      case OpType::Add:
      case OpType::Intersect:
        return secondOp->IsOp(op);
      case OpType::Subtract:
        return secondOp->IsOp(OpType::Add);
      default:
        return false;
    }
  };

  if (secondOp && canInlineSecondOp() && !isReused(secondOp)) {
    auto secondImpl = secondOp->impl_.GetGuard();
    copyChildren(secondImpl->children_, secondOp->transform_);
  } else {
    children.push_back(second);
  }

  return std::make_shared<CsgOpNode>(children, op);
}

std::shared_ptr<CsgNode> CsgOpNode::Transform(const glm::mat4x3 &m) const {
  auto node = std::make_shared<CsgOpNode>();
  node->impl_ = impl_;
  node->transform_ = m * glm::mat4(transform_);
  node->op_ = op_;
  return node;
}

std::shared_ptr<CsgLeafNode> CsgOpNode::ToLeafNode() const {
  if (cache_ != nullptr) return cache_;
  // turn the children into leaf nodes
  GetChildren();
  auto impl = impl_.GetGuard();
  auto &children_ = impl->children_;
  if (children_.size() > 1) {
    switch (op_) {
      case CsgNodeType::Union:
        BatchUnion();
        break;
      case CsgNodeType::Intersection: {
        std::vector<std::shared_ptr<const Manifold::Impl>> impls;
        for (auto &child : children_) {
          impls.push_back(
              std::dynamic_pointer_cast<CsgLeafNode>(child)->GetImpl());
        }
        children_.clear();
        children_.push_back(std::make_shared<CsgLeafNode>(
            BatchBoolean(OpType::Intersect, impls)));
        break;
      };
      case CsgNodeType::Difference: {
        // take the lhs out and treat the remaining nodes as the rhs, perform
        // union optimization for them
        auto lhs = std::dynamic_pointer_cast<CsgLeafNode>(children_.front());
        children_.erase(children_.begin());
        BatchUnion();
        auto rhs = std::dynamic_pointer_cast<CsgLeafNode>(children_.front());
        children_.clear();
        Boolean3 boolean(*lhs->GetImpl(), *rhs->GetImpl(), OpType::Subtract);
        children_.push_back(
            std::make_shared<CsgLeafNode>(std::make_shared<Manifold::Impl>(
                boolean.Result(OpType::Subtract))));
      };
      case CsgNodeType::Leaf:
        // unreachable
        break;
    }
  } else if (children_.size() == 0) {
    return nullptr;
  }
  // children_ must contain only one CsgLeafNode now, and its Transform will
  // give CsgLeafNode as well
  cache_ = std::dynamic_pointer_cast<CsgLeafNode>(
      children_.front()->Transform(transform_));
  return cache_;
}

/**
 * Efficient boolean operation on a set of nodes utilizing commutativity of the
 * operation. Only supports union and intersection.
 */
std::shared_ptr<Manifold::Impl> CsgOpNode::BatchBoolean(
    OpType operation,
    std::vector<std::shared_ptr<const Manifold::Impl>> &results) {
  using SharedImpl = std::variant<std::shared_ptr<const Manifold::Impl>,
                                  std::shared_ptr<Manifold::Impl>>;
  auto getImplPtr = [](const SharedImpl &p) -> const Manifold::Impl * {
    if (std::holds_alternative<std::shared_ptr<const Manifold::Impl>>(p)) {
      return std::get<std::shared_ptr<const Manifold::Impl>>(p).get();
    } else {
      return std::get<std::shared_ptr<Manifold::Impl>>(p).get();
    }
  };
  ASSERT(operation != OpType::Subtract, logicErr,
         "BatchBoolean doesn't support Difference.");
  auto cmpFn = [&getImplPtr](const SharedImpl &a, const SharedImpl &b) {
    return getImplPtr(a)->NumVert() < getImplPtr(b)->NumVert();
  };

  // common cases
  if (results.size() == 0) return std::make_shared<Manifold::Impl>();
  if (results.size() == 1)
    return std::make_shared<Manifold::Impl>(*results.front());
  if (results.size() == 2) {
    Boolean3 boolean(*results[0], *results[1], operation);
    return std::make_shared<Manifold::Impl>(boolean.Result(operation));
  }
#if MANIFOLD_PAR == 'T' && __has_include(<tbb/tbb.h>)
  tbb::task_group group;
  tbb::concurrent_priority_queue<SharedImpl, decltype(cmpFn)> queue(cmpFn);
  for (auto result : results) {
    queue.emplace(result);
  }
  results.clear();
  std::function<void()> process = [&]() {
    while (queue.size() > 1) {
      SharedImpl a, b;
      if (!queue.try_pop(a)) continue;
      if (!queue.try_pop(b)) {
        queue.push(a);
        continue;
      }
      group.run([&, a, b]() {
        const Manifold::Impl *aImpl;
        const Manifold::Impl *bImpl;
        Boolean3 boolean(*getImplPtr(a), *getImplPtr(b), operation);
        queue.emplace(
            std::make_shared<Manifold::Impl>(boolean.Result(operation)));
        return group.run(process);
      });
    }
  };
  group.run_and_wait(process);
  SharedImpl r;
  queue.try_pop(r);
  return std::get<std::shared_ptr<Manifold::Impl>>(r);
#else
  // apply boolean operations starting from smaller meshes
  // the assumption is that boolean operations on smaller meshes is faster,
  // due to less data being copied and processed
  std::make_heap(results.begin(), results.end(), cmpFn);
  while (results.size() > 1) {
    std::pop_heap(results.begin(), results.end(), cmpFn);
    auto a = std::move(results.back());
    results.pop_back();
    std::pop_heap(results.begin(), results.end(), cmpFn);
    auto b = std::move(results.back());
    results.pop_back();
    // boolean operation
    Boolean3 boolean(*a, *b, operation);
    if (results.size() == 0) {
      return std::make_shared<Manifold::Impl>(boolean.Result(operation));
    }
    results.push_back(
        std::make_shared<const Manifold::Impl>(boolean.Result(operation)));
    std::push_heap(results.begin(), results.end(), cmpFn);
  }
  return std::make_shared<Manifold::Impl>(*results.front());
#endif
}

/**
 * Efficient union operation on a set of nodes by doing Compose as much as
 * possible.
 * Note: Due to some unknown issues with `Compose`, we are now doing
 * `BatchBoolean` instead of using `Compose` for non-intersecting manifolds.
 */
void CsgOpNode::BatchUnion() const {
  // INVARIANT: children_ is a vector of leaf nodes
  // this kMaxUnionSize is a heuristic to avoid the pairwise disjoint check
  // with O(n^2) complexity to take too long.
  // If the number of children exceeded this limit, we will operate on chunks
  // with size kMaxUnionSize.
  constexpr int kMaxUnionSize = 1000;
  auto impl = impl_.GetGuard();
  auto &children_ = impl->children_;
  while (children_.size() > 1) {
    const int start = (children_.size() > kMaxUnionSize)
                          ? (children_.size() - kMaxUnionSize)
                          : 0;
    VecDH<Box> boxes;
    boxes.reserve(children_.size() - start);
    for (int i = start; i < children_.size(); i++) {
      boxes.push_back(std::dynamic_pointer_cast<CsgLeafNode>(children_[i])
                          ->GetImpl()
                          ->bBox_);
    }
    const Box *boxesD = boxes.cptrD();
    // partition the children into a set of disjoint sets
    // each set contains a set of children that are pairwise disjoint
    std::vector<VecDH<size_t>> disjointSets;
    for (size_t i = 0; i < boxes.size(); i++) {
      auto lambda = [boxesD, i](const VecDH<size_t> &set) {
        return find_if<decltype(set.end())>(
                   autoPolicy(set.size()), set.begin(), set.end(),
                   CheckOverlap({boxesD, i})) == set.end();
      };
      auto it = std::find_if(disjointSets.begin(), disjointSets.end(), lambda);
      if (it == disjointSets.end()) {
        disjointSets.push_back(std::vector<size_t>{i});
      } else {
        it->push_back(i);
      }
    }
    // compose each set of disjoint children
    std::vector<std::shared_ptr<const Manifold::Impl>> impls(
        disjointSets.size());
    for_each_n_host(
        disjointSets.size() > 1 ? ExecutionPolicy::Par : ExecutionPolicy::Seq,
        countAt(0), disjointSets.size(),
        [children_, &impls, &disjointSets, start](int i) {
          auto set = disjointSets[i];
          if (set.size() == 1) {
            impls[i] = std::dynamic_pointer_cast<CsgLeafNode>(
                           children_[start + set[0]])
                           ->GetImpl();
          } else {
            std::vector<std::shared_ptr<CsgLeafNode>> tmp;
            for (size_t j : set) {
              tmp.push_back(
                  std::dynamic_pointer_cast<CsgLeafNode>(children_[start + j]));
            }
            impls[i] = std::make_shared<const Manifold::Impl>(
                CsgLeafNode::Compose(tmp));
          }
        });

    children_.erase(children_.begin() + start, children_.end());
    children_.push_back(
        std::make_shared<CsgLeafNode>(BatchBoolean(OpType::Add, impls)));
    // move it to the front as we process from the back, and the newly added
    // child should be quite complicated
    std::swap(children_.front(), children_.back());
  }
}

/**
 * Flatten the children to a list of leaf nodes and return them.
 * If forceToLeafNodes is true, the list will be guaranteed to be a list of leaf
 * nodes (i.e. no ops). Otherwise, the list may contain ops. Note that this
 * function will not apply the transform to children, as they may be shared with
 * other nodes.
 */
std::vector<std::shared_ptr<CsgNode>> &CsgOpNode::GetChildren(
    bool forceToLeafNodes) const {
  auto impl = impl_.GetGuard();

  if (forceToLeafNodes && !impl->forcedToLeafNodes_) {
    impl->forcedToLeafNodes_ = true;
    for_each_host(impl->children_.size() > 1 ? ExecutionPolicy::Par
                                             : ExecutionPolicy::Seq,
                  impl->children_.begin(), impl->children_.end(),
                  [](auto &child) {
                    if (child->GetNodeType() != CsgNodeType::Leaf) {
                      child = child->ToLeafNode();
                    }
                  });
  }
  return impl->children_;
}

void CsgOpNode::SetOp(OpType op) {
  switch (op) {
    case OpType::Add:
      op_ = CsgNodeType::Union;
      break;
    case OpType::Subtract:
      op_ = CsgNodeType::Difference;
      break;
    case OpType::Intersect:
      op_ = CsgNodeType::Intersection;
      break;
  }
}

bool CsgOpNode::IsOp(OpType op) {
  switch (op) {
    case OpType::Add:
      return op_ == CsgNodeType::Union;
    case OpType::Subtract:
      return op_ == CsgNodeType::Difference;
    case OpType::Intersect:
      return op_ == CsgNodeType::Intersection;
    default:
      return false;
  }
}

glm::mat4x3 CsgOpNode::GetTransform() const { return transform_; }

}  // namespace manifold

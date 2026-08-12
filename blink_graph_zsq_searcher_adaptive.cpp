/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: blink graph zsq searcher adaptive
 * Create: 2026-03-15
*/

#include "common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec/blink_graph_zsq_searcher_adaptive.h"

#include <algorithm>

namespace Falcon::Common::ShardFormat::FusionIndex {

std::vector<std::pair<float, uint32_t>> BlinkGraphZSQSearcherAdaptive::Search(const float* query, uint32_t topK) const
{
    std::vector<std::pair<float, uint32_t>> resultVec;
    std::vector<float> rdQuery;
    if (rotated_) {
        rdQuery = rotator_->Rotate(query);
        query = rdQuery.data();
    }
    auto* vl = visitedListPool_->GetFreeVisitedList();
    CandidateBuffer<float> candidatePool(std::max(ef_, static_cast<size_t>(topK)));
    candidatePool.Insert(enterPoint_, std::numeric_limits<float>::max());
    std::vector<uint32_t> edges(m_);
    std::vector<float> decodeBuf(dim_);
    while (candidatePool.HasNext()) {
        auto curNode = candidatePool.Pop();
#if defined(USE_X86) || defined(USE_NEON)
        _mm_prefetch(reinterpret_cast<const char*>(GetNeighbor(candidatePool.GetNextId())), _MM_HINT_T0);
#endif
        const uint32_t* nnPtr = GetNeighbor(curNode);
        uint32_t edgeCount = 0;
        for (uint32_t i = 0; i < m_; ++i) {
            uint32_t nn = nnPtr[i];
            CONTINUE_WHEN(vl->Get(nn));
            vl->Set(nn);
#if defined(USE_X86) || defined(USE_NEON)
            _mm_prefetch(reinterpret_cast<const char*>(GetEmbedding(nn)), _MM_HINT_T0);
#endif
            edges[edgeCount++] = nn;
        }
        for (uint32_t i = 0; i < edgeCount; ++i) {
            float nnDist = GetDecodeDist(query, GetEmbedding(edges[i]), decodeBuf.data());
            CONTINUE_WHEN(candidatePool.IsFull(nnDist));
            candidatePool.Insert(edges[i], nnDist);
        }
    }
    std::vector<std::pair<float, uint32_t>> resVec;
    candidatePool.GetResult(resVec);
    float bound = FLT_MAX;
    std::priority_queue<std::pair<float, uint32_t>> result;
    for (auto r : resVec) {
        float dist = GetDecodeDist(query, GetEmbedding(r.second), decodeBuf.data());
        CONTINUE_WHEN(dist >= bound);
        result.emplace(dist, r.second);
        if (result.size() > topK) {
            result.pop();
            bound = result.top().first;
        }
    }
    size_t count = result.size();
    resultVec.resize(count);
    for (size_t i = 0; i < count; ++i) {
        resultVec[count - 1 - i] = result.top();
        result.pop();
    }
    visitedListPool_->ReleaseVisitedList(vl);
    return resultVec;
}

std::vector<std::pair<float, uint32_t>> BlinkGraphZSQSearcherAdaptive::BatchSearch(const float* query,
                                                                                   uint32_t topK) const
{
    std::vector<std::pair<float, uint32_t>> resultVec;
    auto rdQuery = rotator_->Rotate(query);
    BatchQuery<float> queryObj(rdQuery.data(), dim_);
    CandidateBuffer<float> candidatePool(ef_);
    candidatePool.Insert(enterPoint_, std::numeric_limits<float>::max());
    CandidateBuffer<float> resPool(topK);
    auto* vl = visitedListPool_->GetFreeVisitedList();
    std::vector<float> estDist(m_);
    while (candidatePool.HasNext()) {
        float curDist = candidatePool.GetCurDist();
        auto curNode = candidatePool.Pop();
        CONTINUE_WHEN(vl->Get(curNode));
        vl->Set(curNode);
        if (pointMask_[curNode] == ZSQ_MASK_BATCH_POINT) {
            float dist = GetDecodeDist(query, GetEmbedding(curNode));
            queryObj.SetGAdd(dist);
            const auto* batchData = GetBatchData(curNode);
            for (uint32_t i = 0; i < m_; i += PACK_SIZE) {
                ConstBatchDataMap<float> curBatch(batchData, dim_);
                BatchEstimate(curBatch, queryObj, estDist.data() + i);

                batchData += curBatch.BatchSize();
            }
            const uint32_t* nnPtr = GetNeighbor(curNode);
            for (uint32_t i = 0; i < m_; ++i) {
                uint32_t nn = nnPtr[i];
                float distNN = estDist[i];
                CONTINUE_WHEN(candidatePool.IsFull(distNN) || vl->Get(nn));
                candidatePool.Insert(nn, distNN);

#if defined(USE_X86) || defined(USE_NEON)
                _mm_prefetch(reinterpret_cast<const char*>(GetEmbedding(candidatePool.GetNextId())), _MM_HINT_T0);
#endif
            }
            resPool.Insert(curNode, queryObj.GetGAdd());
        } else {
            float cd = 0.0;
            if (pointMask_[curNode] == ZSQ_MASK_NO_BATCH_POINT) {
                cd = curDist;
            } else {
                cd = GetDecodeDist(query, GetEmbedding(curNode));
            }
            const uint32_t* nnPtr = GetNeighbor(curNode);
            for (uint32_t i = 0; i < m_; ++i) {
                uint32_t nn = nnPtr[i];
                CONTINUE_WHEN(vl->Get(nn));
                float dist = GetDecodeDist(query, GetEmbedding(nn));
#if defined(USE_X86) || defined(USE_NEON)
                _mm_prefetch(reinterpret_cast<const char*>(
                                 GetEmbedding(std::min(nnPtr[i + 1], static_cast<uint32_t>(maxElements_ - 1)))),
                             _MM_HINT_T0);
#endif
                CONTINUE_WHEN(candidatePool.IsFull(dist));
                candidatePool.Insert(nn, dist);
            }
            resPool.Insert(curNode, cd);
        }
    }
    visitedListPool_->ReleaseVisitedList(vl);
    resPool.GetResult(resultVec);
    return resultVec;
}

float BlinkGraphZSQSearcherAdaptive::GetDecodeDist(const void* query, const void* emb) const
{
    auto buffer = EmbeddingSQSection::DecodeEmbedding(sqDecoder_, static_cast<const uint8_t*>(emb), dim_);
    const float* embVec = reinterpret_cast<const float*>(buffer.get());
    float dist = fstDistFunc_(query, embVec, distFuncParam_);
    return dist;
}

float BlinkGraphZSQSearcherAdaptive::GetDecodeDist(const void* query, const void* emb, float* decodeBuf) const
{
    sqDecoder_.Decode(static_cast<const uint8_t*>(emb), decodeBuf);
    return fstDistFunc_(query, decodeBuf, distFuncParam_);
}

float BlinkGraphZSQSearcherAdaptive::GetDist(const void* query, const void* emb) const
{
    return static_cast<float>(sqFstdistfunc_(query, emb, distFuncParam_));
}

void BlinkGraphZSQSearcherAdaptive::GetDist4(const void* query, const void* emb0, const void* emb1, const void* emb2,
                                             const void* emb3, float* dists) const
{
    if (sqFstdistfunc4_ != nullptr) {
        uint64_t udists[POS_4];
        sqFstdistfunc4_(query, emb0, emb1, emb2, emb3, distFuncParam_, udists);
        for (uint32_t j = 0; j < POS_4; ++j) {
            dists[j] = static_cast<float>(udists[j]);
        }
        return;
    }
    dists[POS_0] = static_cast<float>(sqFstdistfunc_(query, emb0, distFuncParam_));
    dists[POS_1] = static_cast<float>(sqFstdistfunc_(query, emb1, distFuncParam_));
    dists[POS_2] = static_cast<float>(sqFstdistfunc_(query, emb2, distFuncParam_));
    dists[POS_3] = static_cast<float>(sqFstdistfunc_(query, emb3, distFuncParam_));
}

bool BlinkGraphZSQSearcherAdaptive::Init(const SectionConfig& config, std::shared_ptr<BundleFileReader> reader)
{
    RETURN_FALSE_IF_FALSE_WITH_LOG(BlinkGraphRaBitQSearcherInterface::Init(config, reader), "Init failed");
    SpaceProducerSQ producer;
    auto metricType = config_.fusion_section_config().distance_metric_type();
    sqDistFunc_ = producer.CreateSpaceFunc(dim_, metricType);
    RETURN_FALSE_IF_FALSE_WITH_LOG(sqDistFunc_ != nullptr, "SQ dist func init failed");
    sqFstdistfunc_ = sqDistFunc_->GetDistFunc();
    if (metricType == DistanceMetricType::L2) {
        sqFstdistfunc4_ = Calculator::GetL2DistanceUint64x4Func();
    }
    RETURN_FALSE_IF_FALSE_WITH_LOG(EmbeddingSQSection::InitCodecs(*reader, dim_, sqEncoder_, sqDecoder_),
                                   "Init codec failed");
    return true;
}

} // namespace Falcon::Common::ShardFormat::FusionIndex

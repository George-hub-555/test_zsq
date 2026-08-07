#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <system_error>
#include <utility>
#include <vector>

#include "common/shard_format/bundles/bundle_file_reader.h"
#include "common/shard_format/bundles/bundle_file_writer.h"
#include "common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec/blink_graph_rabitq_builder.h"
#include "common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec/blink_graph_rabitq_searcher_adaptive.h"
#include "common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec/blink_graph_zsq_builder.h"
#include "common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec/blink_graph_zsq_searcher_adaptive.h"
#include "gflags/gflags.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Falcon::Common::ShardFormat::BundleFileReader;
using Falcon::Common::ShardFormat::BundleFileWriter;
using Falcon::Common::ShardFormat::BundleStorageInfo;
using Falcon::Common::ShardFormat::ReaderType;
using namespace Falcon::Common::ShardFormat::FusionIndex;

DEFINE_string(mode, "", "validate, build, or search");
DEFINE_string(variant, "", "rbq or zsq");
DEFINE_string(base_path, "", "SIFT base .fvecs path");
DEFINE_string(query_path, "", "SIFT query .fvecs path");
DEFINE_string(groundtruth_path, "", "SIFT ground-truth .ivecs path");
DEFINE_string(index_path, "", "index input/output path");
DEFINE_string(output_csv, "", "metrics CSV output path");
DEFINE_uint64(base_count, 1000000, "expected base vector count");
DEFINE_uint64(query_count, 10000, "expected query vector count");
DEFINE_uint32(dim, 128, "vector dimension");
DEFINE_uint32(groundtruth_k, 100, "ground-truth neighbors per query");
DEFINE_uint32(thread_count, 16, "OpenMP threads used while building");
DEFINE_uint32(link_range, 32, "graph degree; must be a positive multiple of 32");
DEFINE_uint32(link_candidate_size, 300, "graph construction candidate count");
DEFINE_uint32(build_iter_count, 3, "graph construction iterations");
DEFINE_uint32(batch_size_mb, 1024, "adaptive RaBitQ batch budget in MiB; must be non-zero");
DEFINE_uint32(rotator_type, 0, "0=matrix, 1=FHT/Kac, passed to Falcon RotatorType");
DEFINE_string(search_ranges, "50,100,200,400", "comma-separated ef/search_range values");
DEFINE_string(top_ks, "1,10,100", "comma-separated top-k values");
DEFINE_uint32(warmup_queries, 1000, "warm-up query count for each ef/top-k pair");
DEFINE_uint32(rounds, 5, "timed rounds for each ef/top-k pair");

namespace {

double Seconds(Clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

double Milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

long MaxRssKiB()
{
    struct rusage usage {};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
}

bool EnsureParentDirectory(const std::string& path)
{
    fs::path parent = fs::path(path).parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code error;
    fs::create_directories(parent, error);
    if (error) {
        std::cerr << "Cannot create directory " << parent << ": " << error.message() << std::endl;
        return false;
    }
    return true;
}

bool ReadInt32(std::ifstream& input, int32_t& value)
{
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool ValidateVectorFile(const std::string& path, uint32_t expectedDim, uint64_t expectedCount,
                        const std::string& label)
{
    if (path.empty()) {
        std::cerr << label << " path is empty" << std::endl;
        return false;
    }
    std::error_code error;
    uint64_t size = fs::file_size(path, error);
    if (error) {
        std::cerr << "Cannot stat " << label << " file " << path << ": " << error.message() << std::endl;
        return false;
    }
    const uint64_t rowBytes = sizeof(int32_t) + static_cast<uint64_t>(expectedDim) * sizeof(uint32_t);
    const uint64_t expectedBytes = rowBytes * expectedCount;
    if (size != expectedBytes) {
        std::cerr << label << " size mismatch: actual=" << size << ", expected=" << expectedBytes
                  << " (count=" << expectedCount << ", dim=" << expectedDim << ")" << std::endl;
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open " << label << " file " << path << std::endl;
        return false;
    }
    std::vector<uint64_t> sampleRows = {0, expectedCount / 2, expectedCount - 1};
    for (uint64_t row : sampleRows) {
        input.clear();
        input.seekg(static_cast<std::streamoff>(row * rowBytes));
        int32_t storedDim = 0;
        if (!ReadInt32(input, storedDim) || storedDim != static_cast<int32_t>(expectedDim)) {
            std::cerr << label << " dimension mismatch at row " << row << ": actual=" << storedDim
                      << ", expected=" << expectedDim << std::endl;
            return false;
        }
    }
    std::cout << "VALID " << label << " path=" << path << " count=" << expectedCount << " dim=" << expectedDim
              << " bytes=" << size << std::endl;
    return true;
}

bool LoadFloatVectors(const std::string& path, uint32_t dim, uint64_t count,
                      std::vector<std::vector<float>>& vectors)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open fvecs file " << path << std::endl;
        return false;
    }
    vectors.clear();
    vectors.reserve(count);
    for (uint64_t row = 0; row < count; ++row) {
        int32_t storedDim = 0;
        if (!ReadInt32(input, storedDim) || storedDim != static_cast<int32_t>(dim)) {
            std::cerr << "Invalid fvecs dimension at row " << row << std::endl;
            return false;
        }
        std::vector<float> vector(dim);
        if (!input.read(reinterpret_cast<char*>(vector.data()), static_cast<std::streamsize>(dim * sizeof(float)))) {
            std::cerr << "Truncated fvecs payload at row " << row << std::endl;
            return false;
        }
        vectors.push_back(std::move(vector));
    }
    return input.peek() == std::ifstream::traits_type::eof();
}

bool LoadBaseVectors(const std::string& path, uint32_t dim, uint64_t count,
                     std::vector<std::shared_ptr<float[]>>& vectors)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open base fvecs file " << path << std::endl;
        return false;
    }
    vectors.clear();
    vectors.reserve(count);
    for (uint64_t row = 0; row < count; ++row) {
        int32_t storedDim = 0;
        if (!ReadInt32(input, storedDim) || storedDim != static_cast<int32_t>(dim)) {
            std::cerr << "Invalid base dimension at row " << row << std::endl;
            return false;
        }
        std::shared_ptr<float[]> vector(new float[dim], std::default_delete<float[]>());
        if (!input.read(reinterpret_cast<char*>(vector.get()), static_cast<std::streamsize>(dim * sizeof(float)))) {
            std::cerr << "Truncated base payload at row " << row << std::endl;
            return false;
        }
        vectors.push_back(std::move(vector));
    }
    return input.peek() == std::ifstream::traits_type::eof();
}

bool LoadIntVectors(const std::string& path, uint32_t dim, uint64_t count,
                    std::vector<std::vector<int32_t>>& vectors)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open ivecs file " << path << std::endl;
        return false;
    }
    vectors.clear();
    vectors.reserve(count);
    for (uint64_t row = 0; row < count; ++row) {
        int32_t storedDim = 0;
        if (!ReadInt32(input, storedDim) || storedDim != static_cast<int32_t>(dim)) {
            std::cerr << "Invalid ivecs dimension at row " << row << std::endl;
            return false;
        }
        std::vector<int32_t> vector(dim);
        if (!input.read(reinterpret_cast<char*>(vector.data()), static_cast<std::streamsize>(dim * sizeof(int32_t)))) {
            std::cerr << "Truncated ivecs payload at row " << row << std::endl;
            return false;
        }
        vectors.push_back(std::move(vector));
    }
    return input.peek() == std::ifstream::traits_type::eof();
}

bool ParsePositiveList(const std::string& text, const std::string& name, std::vector<uint32_t>& values)
{
    values.clear();
    std::stringstream stream(text);
    std::string token;
    while (std::getline(stream, token, ',')) {
        try {
            size_t used = 0;
            unsigned long value = std::stoul(token, &used);
            if (used != token.size() || value == 0 || value > UINT32_MAX) {
                throw std::invalid_argument("out of range");
            }
            values.push_back(static_cast<uint32_t>(value));
        } catch (const std::exception&) {
            std::cerr << "Invalid " << name << " entry: '" << token << "'" << std::endl;
            return false;
        }
    }
    if (values.empty()) {
        std::cerr << name << " must not be empty" << std::endl;
        return false;
    }
    return true;
}

bool IsKnownVariant(const std::string& variant)
{
    return variant == "rbq" || variant == "zsq";
}

SectionConfig MakeConfig(uint32_t searchRange)
{
    SectionConfig config;
    auto* fusionConfig = config.mutable_fusion_section_config();
    fusionConfig->set_max_elements(FLAGS_base_count);
    fusionConfig->set_dimension(FLAGS_dim);
    fusionConfig->set_distance_metric_type(DistanceMetricType::L2);
    fusionConfig->set_thread_count(FLAGS_thread_count);
    auto* blinkConfig = fusionConfig->mutable_embedding_section_config()
                            ->mutable_graph_section_config()
                            ->mutable_blink_graph_config();
    blinkConfig->set_link_range(FLAGS_link_range);
    blinkConfig->set_batch_size(FLAGS_batch_size_mb);
    blinkConfig->set_link_candidate_size(FLAGS_link_candidate_size);
    blinkConfig->set_build_iter_count(FLAGS_build_iter_count);
    blinkConfig->set_search_range(searchRange);
    blinkConfig->mutable_rabitq_section_config()->set_ext_bit_len(8);
    blinkConfig->mutable_rabitq_section_config()->set_rotator_type(static_cast<RotatorType>(FLAGS_rotator_type));
    return config;
}

bool ValidateCommonFlags()
{
    if (FLAGS_dim == 0 || FLAGS_base_count == 0 || FLAGS_query_count == 0 || FLAGS_groundtruth_k == 0) {
        std::cerr << "dim/count/groundtruth_k values must be positive" << std::endl;
        return false;
    }
    if (FLAGS_link_range == 0 || FLAGS_link_range % 32 != 0) {
        std::cerr << "link_range must be a positive multiple of 32" << std::endl;
        return false;
    }
    if (FLAGS_batch_size_mb == 0) {
        std::cerr << "batch_size_mb must be non-zero when using the adaptive RBQ/ZSQ searchers" << std::endl;
        return false;
    }
    return true;
}

bool ValidateDataset()
{
    return ValidateVectorFile(FLAGS_base_path, FLAGS_dim, FLAGS_base_count, "base") &&
           ValidateVectorFile(FLAGS_query_path, FLAGS_dim, FLAGS_query_count, "query") &&
           ValidateVectorFile(FLAGS_groundtruth_path, FLAGS_groundtruth_k, FLAGS_query_count, "groundtruth");
}

std::unique_ptr<BlinkGraphRaBitQBuilder> MakeBuilder(const std::string& variant)
{
    if (variant == "zsq") {
        return std::make_unique<BlinkGraphZSQBuilder>();
    }
    return std::make_unique<BlinkGraphRaBitQBuilder>();
}

std::unique_ptr<BlinkGraphRaBitQSearcherInterface> MakeSearcher(const std::string& variant)
{
    if (variant == "zsq") {
        return std::make_unique<BlinkGraphZSQSearcherAdaptive>();
    }
    return std::make_unique<BlinkGraphRaBitQSearcherAdaptive>();
}

bool RunBuild()
{
    if (!IsKnownVariant(FLAGS_variant) || FLAGS_index_path.empty() || FLAGS_output_csv.empty()) {
        std::cerr << "build requires --variant=rbq|zsq, --index_path, and --output_csv" << std::endl;
        return false;
    }
    if (!ValidateVectorFile(FLAGS_base_path, FLAGS_dim, FLAGS_base_count, "base") ||
        !EnsureParentDirectory(FLAGS_index_path) || !EnsureParentDirectory(FLAGS_output_csv)) {
        return false;
    }
    if (fs::exists(FLAGS_index_path) || fs::exists(FLAGS_output_csv)) {
        std::cerr << "Refusing to overwrite existing index or CSV; use a new run directory" << std::endl;
        return false;
    }

    const auto totalStart = Clock::now();
    std::vector<std::shared_ptr<float[]>> base;
    const auto loadStart = Clock::now();
    if (!LoadBaseVectors(FLAGS_base_path, FLAGS_dim, FLAGS_base_count, base)) {
        return false;
    }
    const auto loadEnd = Clock::now();

    SectionConfig config = MakeConfig(200);
    auto builder = MakeBuilder(FLAGS_variant);
    const auto initStart = Clock::now();
    if (!builder->Init(config)) {
        std::cerr << "Falcon builder Init failed" << std::endl;
        return false;
    }
    const auto initEnd = Clock::now();
    const auto buildStart = Clock::now();
    if (!builder->Build(base)) {
        std::cerr << "Falcon builder Build failed" << std::endl;
        return false;
    }
    const auto buildEnd = Clock::now();

    const auto saveStart = Clock::now();
    std::shared_ptr<BundleFileWriter> writer = std::move(BundleFileWriter::New(FLAGS_index_path));
    if (writer == nullptr || !builder->Save(writer)) {
        std::cerr << "Falcon builder Save failed" << std::endl;
        return false;
    }
    writer.reset();
    const auto saveEnd = Clock::now();

    std::error_code error;
    const uint64_t indexBytes = fs::file_size(FLAGS_index_path, error);
    if (error) {
        std::cerr << "Cannot stat generated index: " << error.message() << std::endl;
        return false;
    }
    std::ofstream csv(FLAGS_output_csv);
    if (!csv) {
        std::cerr << "Cannot create CSV " << FLAGS_output_csv << std::endl;
        return false;
    }
    csv << "variant,base_count,dim,thread_count,link_range,link_candidate_size,build_iter_count,batch_size_mb,"
           "rotator_type,load_seconds,init_seconds,build_seconds,save_seconds,total_seconds,index_bytes,max_rss_kib\n";
    csv << FLAGS_variant << ',' << FLAGS_base_count << ',' << FLAGS_dim << ',' << FLAGS_thread_count << ','
        << FLAGS_link_range << ',' << FLAGS_link_candidate_size << ',' << FLAGS_build_iter_count << ','
        << FLAGS_batch_size_mb << ',' << FLAGS_rotator_type << ',' << std::fixed << std::setprecision(6)
        << Seconds(loadEnd - loadStart) << ',' << Seconds(initEnd - initStart) << ','
        << Seconds(buildEnd - buildStart) << ',' << Seconds(saveEnd - saveStart) << ','
        << Seconds(saveEnd - totalStart) << ',' << indexBytes << ',' << MaxRssKiB() << '\n';
    std::cout << "BUILD_OK variant=" << FLAGS_variant << " index=" << FLAGS_index_path
              << " index_bytes=" << indexBytes << " build_seconds=" << Seconds(buildEnd - buildStart)
              << " max_rss_kib=" << MaxRssKiB() << std::endl;
    return true;
}

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::ceil(fraction * values.size())) - 1;
    return values[std::min(index, values.size() - 1)];
}

uint64_t RecallMatches(const std::vector<std::pair<float, uint32_t>>& results,
                       const std::vector<int32_t>& groundTruth, uint32_t topK)
{
    std::set<uint32_t> uniqueResults;
    for (const auto& result : results) {
        uniqueResults.insert(result.second);
    }
    uint64_t matches = 0;
    for (uint32_t i = 0; i < topK; ++i) {
        if (groundTruth[i] >= 0 && uniqueResults.count(static_cast<uint32_t>(groundTruth[i])) != 0) {
            ++matches;
        }
    }
    return matches;
}

bool RunSearch()
{
    if (!IsKnownVariant(FLAGS_variant) || FLAGS_index_path.empty() || FLAGS_output_csv.empty()) {
        std::cerr << "search requires --variant=rbq|zsq, --index_path, and --output_csv" << std::endl;
        return false;
    }
    if (!fs::exists(FLAGS_index_path) || fs::exists(FLAGS_output_csv)) {
        std::cerr << "Index must exist and output CSV must not already exist" << std::endl;
        return false;
    }
    if (!ValidateVectorFile(FLAGS_query_path, FLAGS_dim, FLAGS_query_count, "query") ||
        !ValidateVectorFile(FLAGS_groundtruth_path, FLAGS_groundtruth_k, FLAGS_query_count, "groundtruth") ||
        !EnsureParentDirectory(FLAGS_output_csv)) {
        return false;
    }

    std::vector<uint32_t> searchRanges;
    std::vector<uint32_t> topKs;
    if (!ParsePositiveList(FLAGS_search_ranges, "search_ranges", searchRanges) ||
        !ParsePositiveList(FLAGS_top_ks, "top_ks", topKs)) {
        return false;
    }
    for (uint32_t topK : topKs) {
        if (topK > FLAGS_groundtruth_k) {
            std::cerr << "top_k=" << topK << " exceeds groundtruth_k=" << FLAGS_groundtruth_k << std::endl;
            return false;
        }
    }
    if (FLAGS_rounds == 0) {
        std::cerr << "rounds must be positive" << std::endl;
        return false;
    }

    std::vector<std::vector<float>> queries;
    std::vector<std::vector<int32_t>> groundTruth;
    if (!LoadFloatVectors(FLAGS_query_path, FLAGS_dim, FLAGS_query_count, queries) ||
        !LoadIntVectors(FLAGS_groundtruth_path, FLAGS_groundtruth_k, FLAGS_query_count, groundTruth)) {
        return false;
    }

    auto searcher = MakeSearcher(FLAGS_variant);
    SectionConfig config = MakeConfig(searchRanges.front());
    std::vector<BundleStorageInfo> storageInfo;
    const auto loadStart = Clock::now();
    auto reader = std::make_shared<BundleFileReader>(FLAGS_index_path, ReaderType::MEM_ONLY, storageInfo);
    if (!searcher->Init(config, reader)) {
        std::cerr << "Falcon searcher Init failed" << std::endl;
        return false;
    }
    const double indexLoadMs = Milliseconds(Clock::now() - loadStart);
    std::error_code error;
    const uint64_t indexBytes = fs::file_size(FLAGS_index_path, error);
    if (error) {
        std::cerr << "Cannot stat index: " << error.message() << std::endl;
        return false;
    }

    std::ofstream csv(FLAGS_output_csv);
    if (!csv) {
        std::cerr << "Cannot create CSV " << FLAGS_output_csv << std::endl;
        return false;
    }
    csv << "variant,base_count,query_count,dim,top_k,search_range,rounds,warmup_queries,qps,avg_us,p50_us,"
           "p90_us,p95_us,p99_us,recall_at_k,avg_result_count,index_load_ms,index_bytes,max_rss_kib,checksum\n";

    for (uint32_t searchRange : searchRanges) {
        searcher->SetEf(searchRange);
        for (uint32_t topK : topKs) {
            if (searchRange < topK) {
                std::cout << "SKIP search_range=" << searchRange << " top_k=" << topK
                          << " because search_range < top_k" << std::endl;
                continue;
            }
            const uint32_t warmupCount = std::min<uint64_t>(FLAGS_warmup_queries, FLAGS_query_count);
            uint64_t checksum = 0;
            for (uint32_t i = 0; i < warmupCount; ++i) {
                const auto results = searcher->Search(queries[i].data(), topK);
                checksum += results.size();
                if (!results.empty()) {
                    checksum += results.front().second;
                }
            }

            std::vector<double> latenciesUs;
            latenciesUs.reserve(static_cast<size_t>(FLAGS_query_count) * FLAGS_rounds);
            double totalSearchSeconds = 0.0;
            uint64_t matches = 0;
            uint64_t resultCount = 0;
            for (uint32_t round = 0; round < FLAGS_rounds; ++round) {
                for (uint64_t i = 0; i < FLAGS_query_count; ++i) {
                    const auto start = Clock::now();
                    const auto results = searcher->Search(queries[i].data(), topK);
                    const auto elapsed = Clock::now() - start;
                    const double elapsedSeconds = Seconds(elapsed);
                    totalSearchSeconds += elapsedSeconds;
                    latenciesUs.push_back(elapsedSeconds * 1000000.0);
                    checksum += results.size();
                    if (!results.empty()) {
                        checksum += results.front().second;
                    }
                    if (round == 0) {
                        matches += RecallMatches(results, groundTruth[i], topK);
                        resultCount += results.size();
                    }
                }
            }
            const double searches = static_cast<double>(FLAGS_query_count) * FLAGS_rounds;
            const double qps = searches / totalSearchSeconds;
            const double avgUs = totalSearchSeconds * 1000000.0 / searches;
            const double recall = static_cast<double>(matches) /
                                  (static_cast<double>(FLAGS_query_count) * static_cast<double>(topK));
            const double avgResultCount = static_cast<double>(resultCount) / FLAGS_query_count;
            const long maxRss = MaxRssKiB();
            csv << FLAGS_variant << ',' << FLAGS_base_count << ',' << FLAGS_query_count << ',' << FLAGS_dim << ','
                << topK << ',' << searchRange << ',' << FLAGS_rounds << ',' << warmupCount << ',' << std::fixed
                << std::setprecision(6) << qps << ',' << avgUs << ',' << Percentile(latenciesUs, 0.50) << ','
                << Percentile(latenciesUs, 0.90) << ',' << Percentile(latenciesUs, 0.95) << ','
                << Percentile(latenciesUs, 0.99) << ',' << recall << ',' << avgResultCount << ',' << indexLoadMs << ','
                << indexBytes << ',' << maxRss << ',' << checksum << '\n';
            csv.flush();
            std::cout << "SEARCH_OK variant=" << FLAGS_variant << " top_k=" << topK
                      << " search_range=" << searchRange << " qps=" << qps << " avg_us=" << avgUs
                      << " recall=" << recall << " max_rss_kib=" << maxRss << std::endl;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (!ValidateCommonFlags()) {
        return 2;
    }
    if (FLAGS_mode == "validate") {
        return ValidateDataset() ? 0 : 1;
    }
    if (FLAGS_mode == "build") {
        return RunBuild() ? 0 : 1;
    }
    if (FLAGS_mode == "search") {
        return RunSearch() ? 0 : 1;
    }
    std::cerr << "--mode must be validate, build, or search" << std::endl;
    return 2;
}

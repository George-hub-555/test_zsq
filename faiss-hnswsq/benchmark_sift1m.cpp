#include <faiss/IndexHNSW.h>
#include <faiss/impl/ScalarQuantizer.h>

#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
struct Matrix {
    size_t rows = 0;
    size_t columns = 0;
    std::vector<T> values;

    const T* row(size_t index) const {
        return values.data() + index * columns;
    }
};

struct Options {
    std::filesystem::path dataset = "dataset";
    std::filesystem::path output_dir = "results";
    std::vector<int> ef_values = {
            10, 15, 20, 30, 40, 50, 75, 100, 150, 200, 300, 400, 800};
    int top_k = 10;
    int M = 16;
    int ef_construction = 100;
    int repeats = 5;
    int warmup_queries = 1000;
    int query_count = 0;
    int threads = 1;
    std::string sq = "SQ8";
};

std::vector<int> parse_int_list(const std::string& text) {
    std::vector<int> result;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            result.push_back(std::stoi(item));
        }
    }
    return result;
}

// Maps an SQ string (e.g. "SQ8") to the Faiss scalar quantizer type.
// "Flat" disables quantization and falls back to IndexHNSWFlat.
faiss::ScalarQuantizer::QuantizerType parse_sq_type(const std::string& sq) {
    static const std::map<std::string, faiss::ScalarQuantizer::QuantizerType>
            table = {
                    {"SQ8", faiss::ScalarQuantizer::QT_8bit},
                    {"SQ4", faiss::ScalarQuantizer::QT_4bit},
                    {"SQ6", faiss::ScalarQuantizer::QT_6bit},
                    {"SQfp16", faiss::ScalarQuantizer::QT_fp16},
                    {"SQbf16", faiss::ScalarQuantizer::QT_bf16},
            };
    const auto it = table.find(sq);
    if (it == table.end()) {
        throw std::runtime_error(
                "unknown SQ type: " + sq +
                " (supported: SQ8, SQ6, SQ4, SQfp16, SQbf16, Flat)");
    }
    return it->second;
}

void print_usage(const char* executable) {
    std::cout
            << "Usage: " << executable << " [options]\n"
            << "  --dataset PATH          directory containing SIFT1M files\n"
            << "  --output-dir PATH       default: results\n"
            << "  --ef-search LIST        comma-separated efSearch values\n"
            << "  --top-k N               default: 10\n"
            << "  --M N                   default: 16\n"
            << "  --ef-construction N     default: 100\n"
            << "  --repeats N             default: 5\n"
            << "  --warmup-queries N      default: 1000\n"
            << "  --query-count N         0 means all queries (default)\n"
            << "  --threads N             default: 1\n"
            << "  --sq NAME               SQ8/SQ6/SQ4/SQfp16/SQbf16/Flat, "
               "default: SQ8\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (i + 1 >= argc) {
            throw std::runtime_error("missing value for " + key);
        }
        const std::string value = argv[++i];
        if (key == "--dataset") {
            options.dataset = value;
        } else if (key == "--output-dir") {
            options.output_dir = value;
        } else if (key == "--ef-search") {
            options.ef_values = parse_int_list(value);
        } else if (key == "--top-k") {
            options.top_k = std::stoi(value);
        } else if (key == "--M") {
            options.M = std::stoi(value);
        } else if (key == "--ef-construction") {
            options.ef_construction = std::stoi(value);
        } else if (key == "--repeats") {
            options.repeats = std::stoi(value);
        } else if (key == "--warmup-queries") {
            options.warmup_queries = std::stoi(value);
        } else if (key == "--query-count") {
            options.query_count = std::stoi(value);
        } else if (key == "--threads") {
            options.threads = std::stoi(value);
        } else if (key == "--sq") {
            options.sq = value;
        } else {
            throw std::runtime_error("unknown option: " + key);
        }
    }
    if (options.ef_values.empty() || options.top_k <= 0 || options.M <= 0 ||
        options.ef_construction <= 0 || options.repeats <= 0 ||
        options.warmup_queries < 0 || options.query_count < 0 ||
        options.threads <= 0) {
        throw std::runtime_error("invalid non-positive benchmark option");
    }
    for (int ef : options.ef_values) {
        if (ef < options.top_k) {
            throw std::runtime_error("each efSearch must be >= top-k");
        }
    }
    return options;
}

template <typename T>
Matrix<T> read_vecs(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    int32_t dimension = 0;
    input.read(reinterpret_cast<char*>(&dimension), sizeof(dimension));
    if (!input || dimension <= 0) {
        throw std::runtime_error("invalid header in " + path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff file_bytes = input.tellg();
    const std::streamoff record_bytes =
            sizeof(int32_t) + static_cast<std::streamoff>(dimension) * sizeof(T);
    if (file_bytes <= 0 || file_bytes % record_bytes != 0) {
        throw std::runtime_error("invalid file size for " + path.string());
    }

    Matrix<T> result;
    result.rows = static_cast<size_t>(file_bytes / record_bytes);
    result.columns = static_cast<size_t>(dimension);
    result.values.resize(result.rows * result.columns);
    input.seekg(0, std::ios::beg);
    for (size_t row = 0; row < result.rows; ++row) {
        int32_t row_dimension = 0;
        input.read(reinterpret_cast<char*>(&row_dimension), sizeof(row_dimension));
        if (!input || row_dimension != dimension) {
            throw std::runtime_error("inconsistent dimension in " + path.string());
        }
        input.read(
                reinterpret_cast<char*>(result.values.data() + row * result.columns),
                static_cast<std::streamsize>(result.columns * sizeof(T)));
        if (!input) {
            throw std::runtime_error("short read from " + path.string());
        }
    }
    return result;
}

double elapsed_seconds(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

double recall_at_k(
        const std::vector<faiss::idx_t>& labels,
        const Matrix<int32_t>& ground_truth,
        size_t query_count,
        int k) {
    uint64_t hits = 0;
    for (size_t query = 0; query < query_count; ++query) {
        const int32_t* expected = ground_truth.row(query);
        for (int position = 0; position < k; ++position) {
            const faiss::idx_t id = labels[query * static_cast<size_t>(k) + position];
            if (id >= 0 &&
                std::find(expected, expected + k, static_cast<int32_t>(id)) !=
                        expected + k) {
                ++hits;
            }
        }
    }
    return static_cast<double>(hits) /
            static_cast<double>(query_count * static_cast<size_t>(k));
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 == 0
            ? (values[middle - 1] + values[middle]) * 0.5
            : values[middle];
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        omp_set_num_threads(options.threads);

        std::cout << "Loading SIFT1M..." << std::endl;
        const Matrix<float> base =
                read_vecs<float>(options.dataset / "sift_base.fvecs");
        const Matrix<float> queries =
                read_vecs<float>(options.dataset / "sift_query.fvecs");
        const Matrix<int32_t> ground_truth =
                read_vecs<int32_t>(options.dataset / "sift_groundtruth.ivecs");
        if (base.columns != queries.columns || queries.rows != ground_truth.rows) {
            throw std::runtime_error("SIFT1M matrix shapes are inconsistent");
        }
        if (static_cast<size_t>(options.top_k) > ground_truth.columns) {
            throw std::runtime_error("top-k exceeds ground-truth width");
        }
        const size_t query_count = options.query_count == 0
                ? queries.rows
                : std::min(queries.rows, static_cast<size_t>(options.query_count));

        std::cout << "Building Faiss IndexHNSW" << options.sq << ": d="
                  << base.columns << ", nb=" << base.rows << ", M=" << options.M
                  << ", efConstruction=" << options.ef_construction << std::endl;
        const auto build_begin = Clock::now();
        faiss::IndexHNSWSQ index(
                static_cast<int>(base.columns),
                parse_sq_type(options.sq),
                options.M,
                faiss::METRIC_L2);
        index.hnsw.efConstruction = options.ef_construction;
        // SQ needs a training pass to fit the quantization range.
        // Train on the first 100k base vectors (SIFT1M convention).
        const faiss::idx_t n_train =
                std::min<faiss::idx_t>(100000, static_cast<faiss::idx_t>(base.rows));
        index.train(n_train, base.values.data());
        index.add(static_cast<faiss::idx_t>(base.rows), base.values.data());
        const double build_seconds = elapsed_seconds(build_begin);

        std::filesystem::create_directories(options.output_dir);
        std::ofstream raw(options.output_dir / "faiss_hnsw_raw.csv");
        std::ofstream summary(options.output_dir / "faiss_hnsw_summary.csv");
        if (!raw || !summary) {
            throw std::runtime_error("cannot create output CSV files");
        }
        raw << "ef_search,repeat,elapsed_seconds,qps,top_k,recall_at_k,threads,M,"
               "ef_construction,query_count,build_seconds,sq\n";
        summary << "ef_search,median_qps,top_k,recall_at_k,threads,M,ef_construction,"
                   "query_count,repeats,build_seconds,sq\n";
        raw << std::setprecision(10);
        summary << std::setprecision(10);

        for (int ef : options.ef_values) {
            index.hnsw.efSearch = ef;
            const size_t warmup_count = std::min(
                    query_count, static_cast<size_t>(options.warmup_queries));
            if (warmup_count > 0) {
                std::vector<float> distances(warmup_count * options.top_k);
                std::vector<faiss::idx_t> labels(warmup_count * options.top_k);
                index.search(
                        static_cast<faiss::idx_t>(warmup_count),
                        queries.values.data(),
                        options.top_k,
                        distances.data(),
                        labels.data());
            }

            std::vector<double> qps_values;
            double recall = 0.0;
            for (int repeat = 0; repeat < options.repeats; ++repeat) {
                std::vector<float> distances(query_count * options.top_k);
                std::vector<faiss::idx_t> labels(query_count * options.top_k);
                const auto search_begin = Clock::now();
                index.search(
                        static_cast<faiss::idx_t>(query_count),
                        queries.values.data(),
                        options.top_k,
                        distances.data(),
                        labels.data());
                const double seconds = elapsed_seconds(search_begin);
                const double qps = static_cast<double>(query_count) / seconds;
                if (repeat == 0) {
                    recall = recall_at_k(
                            labels, ground_truth, query_count, options.top_k);
                }
                qps_values.push_back(qps);
                raw << ef << ',' << repeat << ',' << seconds << ',' << qps << ','
                    << options.top_k << ',' << recall << ',' << options.threads << ',' << options.M << ','
                    << options.ef_construction << ',' << query_count << ','
                    << build_seconds << ',' << options.sq << '\n';
            }
            const double median_qps = median(qps_values);
            summary << ef << ',' << median_qps << ',' << options.top_k << ',' << recall << ','
                    << options.threads << ',' << options.M << ','
                    << options.ef_construction << ',' << query_count << ','
                    << options.repeats << ',' << build_seconds << ',' << options.sq
                    << '\n';
            raw.flush();
            summary.flush();
            std::cout << "efSearch=" << ef << ", QPS=" << median_qps
                      << ", Recall@" << options.top_k << '=' << recall << std::endl;
        }
        std::cout << "CSV written to " << options.output_dir << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << std::endl;
        return 1;
    }
}

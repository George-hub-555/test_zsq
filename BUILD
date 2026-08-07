cc_binary(
    name = "zsq_benchmark",
    srcs = ["zsq_benchmark.cpp"],
    visibility = ["//visibility:public"],
    deps = [
        "//common/shard_format/bundles:bundle_file_reader",
        "//common/shard_format/bundles:bundle_file_writer",
        "//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_erq_builder",
        "//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_erq_searcher_adaptive",
        "//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_rabitq_builder",
        "//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_rabitq_searcher_adaptive",
        "//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_zsq_builder",
        "//common/shard_format/fusion_index/embedding_index/quantizer_index/rabitq_index/rabitq_codec:blink_graph_zsq_searcher_adaptive",
        "@com_github_gflags_gflags//:gflags",
    ],
)

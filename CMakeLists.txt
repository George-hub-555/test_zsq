cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(faiss_hnsw_sift1m_benchmark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build only the generic CPU Faiss library needed by this benchmark.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_GPU OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_EXTRAS OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_MKL OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_SVS OFF CACHE BOOL "" FORCE)
set(FAISS_OPT_LEVEL generic CACHE STRING "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../faiss"
    "${CMAKE_CURRENT_BINARY_DIR}/faiss"
)

add_executable(faiss_hnsw_sift1m benchmark_sift1m.cpp)
target_link_libraries(faiss_hnsw_sift1m PRIVATE faiss OpenMP::OpenMP_CXX)

install(TARGETS faiss_hnsw_sift1m RUNTIME DESTINATION bin)
install(PROGRAMS plot_qps_recall.py DESTINATION bin)

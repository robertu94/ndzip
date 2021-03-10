#include "ubench.hh"

#include <ndzip/gpu_encoder.inl>
#include <test/test_utils.hh>

using namespace ndzip;
using namespace ndzip::detail;
using namespace ndzip::detail::gpu;
using sam = sycl::access::mode;


#define ALL_PROFILES \
    (profile<float, 1>), (profile<float, 2>), (profile<float, 3>), (profile<double, 1>), \
            (profile<double, 2>), (profile<double, 3>)

// Kernel names (for profiler)
template<typename>
class load_hypercube_kernel;
template<typename>
class block_transform_reference_kernel;
template<typename>
class block_forward_transform_kernel;
template<typename>
class block_inverse_transform_kernel;
template<typename>
class encode_reference_kernel;
template<typename>
class chunk_transpose_write_kernel;
template<typename>
class chunk_transpose_read_kernel;
template<typename>
class chunk_compact_kernel;


TEMPLATE_TEST_CASE("Loading", "[load]", ALL_PROFILES) {
    using data_type = typename TestType::data_type;
    using bits_type = typename TestType::bits_type;
    using hc_layout = gpu::hypercube_layout<TestType::dimensions, gpu::forward_transform_tag>;

    constexpr unsigned dimensions = TestType::dimensions;
    constexpr index_type n_blocks = 16384;

    const auto grid_extent = [] {
        extent<TestType::dimensions> grid_extent;
        const auto n_blocks_regular = static_cast<index_type>(pow(n_blocks, 1.f / dimensions));
        auto n_blocks_to_distribute = n_blocks;
        for (unsigned d = 0; d < dimensions; ++d) {
            auto n_blocks_this_dim = std::min(n_blocks_regular, n_blocks_to_distribute);
            grid_extent[d] = n_blocks_this_dim * TestType::hypercube_side_length + 3 /* border */;
            n_blocks_to_distribute /= n_blocks_this_dim;
        }
        assert(n_blocks_to_distribute == 0);
        return grid_extent;
    }();

    const auto data = make_random_vector<data_type>(num_elements(grid_extent));
    sycl::buffer<data_type> data_buffer(data.data(), data.size());

    SYCL_BENCHMARK("Load hypercube")(sycl::queue & q) {
        constexpr auto hc_size = ipow(TestType::hypercube_side_length, TestType::dimensions);

        sycl::buffer<bits_type> out(n_blocks * hc_size);
        return q.submit([&](sycl::handler &cgh) {
            auto data_acc = data_buffer.template get_access<sam::read>(cgh);
            cgh.parallel<load_hypercube_kernel<TestType>>(sycl::range<1>{n_blocks},
                    sycl::range<1>{hypercube_group_size},
                    [=](hypercube_group grp, sycl::physical_item<1>) {
                        hypercube_memory<bits_type, hc_layout> lm{grp};
                        gpu::hypercube_ptr<TestType, gpu::forward_transform_tag> hc{lm()};
                        detail::gpu::index_type hc_index = grp.get_id(0);
                        slice<const data_type, dimensions> data{
                                data_acc.get_pointer(), grid_extent};

                        load_hypercube(grp, hc_index, data, hc);

                        black_hole(hc.memory);
                    });
        });
    };
}


TEMPLATE_TEST_CASE("Block transform", "[transform]", ALL_PROFILES) {
    using bits_type = typename TestType::bits_type;
    constexpr index_type n_blocks = 16384;

    SYCL_BENCHMARK("Reference: rotate only")(sycl::queue & q) {
        using hc_layout = gpu::hypercube_layout<TestType::dimensions, gpu::forward_transform_tag>;
        constexpr auto hc_size = ipow(TestType::hypercube_side_length, TestType::dimensions);

        return q.submit([&](sycl::handler &cgh) {
            cgh.parallel<block_transform_reference_kernel<TestType>>(sycl::range<1>{n_blocks},
                    sycl::range<1>{hypercube_group_size},
                    [=](hypercube_group grp, sycl::physical_item<1>) {
                        hypercube_memory<bits_type, hc_layout> lm{grp};
                        gpu::hypercube_ptr<TestType, gpu::forward_transform_tag> hc{lm()};
                        grp.distribute_for(hc_size, [&](index_type i) { hc.store(i, i); });
                        black_hole(hc.memory);
                    });
        });
    };

    SYCL_BENCHMARK("Forward transform")(sycl::queue & q) {
        using hc_layout = gpu::hypercube_layout<TestType::dimensions, gpu::forward_transform_tag>;
        constexpr auto hc_size = ipow(TestType::hypercube_side_length, TestType::dimensions);

        return q.submit([&](sycl::handler &cgh) {
            cgh.parallel<block_forward_transform_kernel<TestType>>(sycl::range<1>{n_blocks},
                    sycl::range<1>{hypercube_group_size},
                    [=](hypercube_group grp, sycl::physical_item<1>) {
                        hypercube_memory<bits_type, hc_layout> lm{grp};
                        gpu::hypercube_ptr<TestType, gpu::forward_transform_tag> hc{lm()};
                        grp.distribute_for(hc_size, [&](index_type i) { hc.store(i, i); });
                        forward_block_transform(grp, hc);
                        black_hole(hc.memory);
                    });
        });
    };

    SYCL_BENCHMARK("Inverse transform")(sycl::queue & q) {
        using hc_layout = gpu::hypercube_layout<TestType::dimensions, gpu::inverse_transform_tag>;
        constexpr auto hc_size = ipow(TestType::hypercube_side_length, TestType::dimensions);

        return q.submit([&](sycl::handler &cgh) {
            cgh.parallel<block_inverse_transform_kernel<TestType>>(sycl::range<1>{n_blocks},
                    sycl::range<1>{hypercube_group_size},
                    [=](hypercube_group grp, sycl::physical_item<1>) {
                        hypercube_memory<bits_type, hc_layout> lm{grp};
                        gpu::hypercube_ptr<TestType, gpu::inverse_transform_tag> hc{lm()};
                        grp.distribute_for(hc_size, [&](index_type i) { hc.store(i, i); });
                        inverse_block_transform(grp, hc);
                        black_hole(hc.memory);
                    });
        });
    };
}


// Impact of dimensionality should not be that large, but the hc padding could hold surprises
TEMPLATE_TEST_CASE("Chunk encoding", "[encode]", ALL_PROFILES) {
    constexpr index_type n_blocks = 16384;
    using bits_type = typename TestType::bits_type;
    using hc_layout = gpu::hypercube_layout<TestType::dimensions, gpu::forward_transform_tag>;
    constexpr auto hc_size = ipow(TestType::hypercube_side_length, TestType::dimensions);
    constexpr auto warps_per_hc = hc_size / warp_size;

    SYCL_BENCHMARK("Reference: serialize")(sycl::queue & q) {
      return q.submit([&](sycl::handler &cgh) {
        cgh.parallel<encode_reference_kernel<TestType>>(sycl::range<1>{n_blocks},
                sycl::range<1>{hypercube_group_size},
                [=](hypercube_group grp, sycl::physical_item<1>) {
                  hypercube_memory<bits_type, hc_layout> lm{grp};
                  gpu::hypercube_ptr<TestType, gpu::forward_transform_tag> hc{lm()};
                  grp.distribute_for(hc_size, [&](index_type i) { hc.store(i, i); });
                  black_hole(hc.memory);
                });
      });
    };

    sycl::buffer<bits_type> columns(n_blocks * hc_size);
    sycl::buffer<bits_type> heads(n_blocks * warps_per_hc);
    sycl::buffer<index_type> lengths(ceil(1 + n_blocks * warps_per_hc,
            gpu::hierarchical_inclusive_scan_granularity));

    SYCL_BENCHMARK("Transpose chunks")(sycl::queue & q) {
      return q.submit([&](sycl::handler &cgh) {
        auto c = columns.template get_access<sam::discard_write>(cgh);
        auto h = heads.template get_access<sam::discard_write>(cgh);
        auto l = lengths.template get_access<sam::discard_write>(cgh);
        cgh.parallel<chunk_transpose_write_kernel<TestType>>(sycl::range<1>{n_blocks},
                sycl::range<1>{hypercube_group_size},
                [=](hypercube_group grp, sycl::physical_item<1> phys_idx) {
                  hypercube_memory<bits_type, hc_layout> lm{grp};
                  gpu::hypercube_ptr<TestType, gpu::forward_transform_tag> hc{lm()};
                  grp.distribute_for(hc_size, [&](index_type i) { hc.store(i, i * 199); });
                  const auto hc_index = grp.get_id(0);
                  write_transposed_chunks(grp, hc, &h[hc_index * warps_per_hc],
                          &c[hc_index * hc_size], &l[1 + hc_index * warps_per_hc]);
                  // hack
                  if (phys_idx.get_global_linear_id() == 0) {
                      grp.single_item([&] { l[0] = 0; });
                  }
                });
      });
    };

    {
        sycl::queue q;
        gpu::hierarchical_inclusive_scan(q, lengths, sycl::plus<gpu::index_type>{});
    }

    sycl::buffer<bits_type> stream(n_blocks * (hc_size + hc_size / warp_size));

    SYCL_BENCHMARK("Compact transposed")(sycl::queue & q) {
      return q.submit([&](sycl::handler &cgh) {
        auto c = columns.template get_access<sam::read>(cgh);
        auto h = heads.template get_access<sam::read>(cgh);
        auto l = lengths.template get_access<sam::read>(cgh);
        auto s = stream.template get_access<sam::discard_write>(cgh);
        constexpr size_t group_size = 1024;
        cgh.parallel<chunk_compact_kernel<TestType>>(
                sycl::range<1>{hc_size / group_size * n_blocks}, sycl::range<1>{group_size},
                [=](sycl::group<1> grp, sycl::physical_item<1>) {
                  compact_chunks<TestType>(
                          grp, static_cast<const bits_type *>(h.get_pointer()),
                          static_cast<const bits_type *>(c.get_pointer()),
                          static_cast<const index_type *>(l.get_pointer()),
                          static_cast<bits_type *>(s.get_pointer()));
                });
      });
    };
}


// Impact of dimensionality should not be that large, but the hc padding could hold surprises
TEMPLATE_TEST_CASE("Chunk decoding", "[decode]", ALL_PROFILES) {
    constexpr index_type n_blocks = 16384;
    using bits_type = typename TestType::bits_type;
    using hc_layout = gpu::hypercube_layout<TestType::dimensions, gpu::inverse_transform_tag>;
    constexpr auto hc_size = ipow(TestType::hypercube_side_length, TestType::dimensions);

    sycl::buffer<bits_type> columns(n_blocks * hc_size);
    sycl::queue{}.submit([&](sycl::handler &cgh) {
        cgh.fill(columns.template get_access<sam::discard_write>(cgh),
                static_cast<bits_type>(7948741984121192831 /* whatever */));
    });

    SYCL_BENCHMARK("Read and transpose")(sycl::queue & q) {
        return q.submit([&](sycl::handler &cgh) {
            auto c = columns.template get_access<sam::read>(cgh);
            cgh.parallel<chunk_transpose_read_kernel<TestType>>(sycl::range<1>{n_blocks},
                    sycl::range<1>{hypercube_group_size},
                    [=](hypercube_group grp, sycl::physical_item<1>) {
                        hypercube_memory<bits_type, hc_layout> lm{grp};
                        gpu::hypercube_ptr<TestType, gpu::inverse_transform_tag> hc{lm()};
                        const auto hc_index = grp.get_id(0);
                        const bits_type *column = c.get_pointer();
                        read_transposed_chunks(grp, hc, &column[hc_index * hc_size]);
                    });
        });
    };
}

#include "common.h"

constexpr size_t THREADS_NO = std::min(max_no_threads, 8);

constexpr size_t elems_per_thread = 100000;
constexpr size_t key_range = elems_per_thread * THREADS_NO * 2;

// TODO: document the test

// This test checks insert_unique method under pressure.
// Test checks for missing data and changed/overwriten data.
int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);

  memory_check(THREADS_NO, [] {
    map_t skiplist;

    auto futures = run<std::vector<size_t>>(
        THREADS_NO, skiplist, [](auto acc, auto index) {
          auto rand = rand_gen(key_range);
          long long downcount = elems_per_thread;
          std::vector<size_t> owned;
          auto inserter =
              insert_try<size_t, size_t, map_t>(acc, downcount, owned);

          do {
            inserter(rand(), index);
          } while (downcount > 0);

          check_present_same<map_t>(acc, index, owned);
          return owned;
        });

    auto accessor = skiplist.access();
    for (auto &owned : collect(futures)) {
      check_present_same<map_t>(accessor, owned);
    }

    check_size<map_t>(accessor, THREADS_NO * elems_per_thread);
    check_order<map_t>(accessor);
  });
}

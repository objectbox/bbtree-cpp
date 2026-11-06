#include "bench_program.hpp"

#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "cell_runner.hpp"
#include "command_line.hpp"
#include "dataset_fetch.hpp"
#include "datasets.hpp"
#include "run_comparison.hpp"
#include "self_check.hpp"
#include "suite_runner.hpp"

namespace bbtree_bench {

namespace {

// What each invocation does.
struct invocation_runner {
  run_mode mode;

  void operator()(const invocation::help&) const { std::cout << invocation::usage() << '\n'; }
  void operator()(const invocation::verify_data&) const { data_directory::from_environment().verify(); }
  void operator()(const invocation::fetch_data&) const {
    dataset_fetch::fetch_into(data_directory::from_environment());
  }
  void operator()(const invocation::self_check&) const { self_check::run(); }
  void operator()(const suite_request& request) const { suite_runner(request, mode).run(); }
  void operator()(const comparison_request& request) const {
    std::cout << run_comparison::load(request).markdown();
  }
  void operator()(const cell_request& request) const { cell_run(request, mode).measure_and_report(); }
};

}  // namespace

int bench_program::run(int argc, char** argv, run_mode mode) {
  const std::vector<std::string> arguments(argv + 1, argv + argc);
  try {
    invocation::request request = [&] {
      try {
        return invocation::parse(arguments);
      } catch (const bench_error& problem) {
        throw bench_error(std::string(problem.what()) + "\n\n" + invocation::usage());
      }
    }();
    std::visit(invocation_runner{mode}, request);
    return 0;
  } catch (const std::exception& problem) {
    std::cout.flush();
    std::cerr << "error: " << problem.what() << '\n';
    return 1;
  }
}

}  // namespace bbtree_bench

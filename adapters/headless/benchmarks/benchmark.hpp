// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace LotusHeadless {

// Separate invocation from renderer correctness evidence. Returns 77 for an
// explained unavailable GPU capability, 1 for failures and 2 for CLI errors.
int BenchmarkMain(int argc, char** argv);

} // namespace LotusHeadless

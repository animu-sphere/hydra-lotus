# SPDX-License-Identifier: Apache-2.0
# Test tiers (docs/design/TEST_POLICY.md). Every CTest carries exactly one
# tier label; lotus-test-tiers rejects a test without one.
include_guard(GLOBAL)

# lotus_test_tier(<fast|gpu|research> <test>...)
#
# Appends the tier label, so a test keeps any area labels it already has.
function(lotus_test_tier tier)
  if(NOT tier MATCHES "^(fast|gpu|research)$")
    message(FATAL_ERROR "Unknown test tier: ${tier}")
  endif()
  set_property(TEST ${ARGN} APPEND PROPERTY LABELS ${tier})
endfunction()

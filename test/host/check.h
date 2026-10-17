#pragma once

// The host tests' assertion: on failure, says where and what, and the test
// fails; the test carries on. A macro, for the file, line and text, so
// it's a header: macros don't travel through an import.
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            sunsim::testing::fail(__FILE__, __LINE__, #cond);                                                             \
        }                                                                                                              \
    } while (0)

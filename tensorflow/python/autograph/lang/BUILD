load("//tensorflow:tensorflow.bzl", "py_test")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    licenses = ["notice"],
)

filegroup(
    name = "all_files",
    srcs = glob(
        ["**/*"],
        exclude = [
            "**/METADATA",
            "**/OWNERS",
        ],
    ),
    visibility = ["//visibility:private"],
)

py_library(
    name = "lang",
    srcs = [
        "directives.py",
        "special_functions.py",
    ],
    srcs_version = "PY3",
    visibility = ["//tensorflow:__subpackages__"],
    deps = [
        "//tensorflow/python/autograph/operators",
    ],
)

py_test(
    name = "special_functions_test",
    srcs = ["special_functions_test.py"],
    python_version = "PY3",
    srcs_version = "PY3",
    deps = [
        ":lang",
        "//tensorflow/python:client_testlib",
    ],
)

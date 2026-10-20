"""Finds libsystemd with pkg-config and exposes it as @libsystemd//:sdbus.

When it is missing, @libsystemd//:sdbus is an empty library and CCBUS_HAVE_SDBUS
stays undefined, so the TUI builds without desktop notifications.
"""

def _libsystemd_impl(rctx):
    r = rctx.execute(["pkg-config", "--libs", "libsystemd"])
    if r.return_code == 0:
        libs = r.stdout.strip().split(" ")
        body = """cc_library(
    name = "sdbus",
    defines = ["CCBUS_HAVE_SDBUS"],
    linkopts = {libs},
    visibility = ["//visibility:public"],
)
""".format(libs = repr([l for l in libs if l]))
    else:
        print("libsystemd not found: ccbus-tui builds without desktop notifications")
        body = """cc_library(name = "sdbus", visibility = ["//visibility:public"])
"""
    rctx.file("BUILD.bazel", 'load("@rules_cc//cc:defs.bzl", "cc_library")\n\n' + body)

libsystemd = repository_rule(
    implementation = _libsystemd_impl,
    local = True,
    configure = True,
)

def _systemd_ext_impl(_mctx):
    libsystemd(name = "libsystemd")

systemd = module_extension(implementation = _systemd_ext_impl)

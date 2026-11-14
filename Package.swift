// swift-tools-version: 5.9
import PackageDescription

let cFlags = ["-Wall", "-Wextra"]

let package = Package(
    name: "RecoveryC",
    platforms: [.macOS(.v12)],
    products: [
        .library(name: "recovery_c", type: .static, targets: ["recovery_c"]),
        .executable(name: "night_example", targets: ["night_example"]),
        .executable(name: "test_recovery", targets: ["test_recovery"]),
        .executable(name: "test_parity", targets: ["test_parity"]),
    ],
    targets: [
        .target(
            name: "recovery_c",
            path: "recovery_c",
            exclude: [".abacus.donotdelete", "Makefile", "README.md", "build", "examples", "tests", "tools"],
            sources: [
                "src/rc_baseline.c",
                "src/rc_config.c",
                "src/rc_engine.c",
                "src/rc_explain.c",
                "src/rc_night.c",
                "src/rc_rr.c",
                "src/rc_scoring.c",
                "src/rc_segments.c",
                "src/rc_sleep.c",
                "src/rc_stages.c",
                "src/rc_util.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("src"),
                .unsafeFlags(cFlags),
            ],
            linkerSettings: [.linkedLibrary("m")]
        ),
        .target(
            name: "recovery_json",
            path: "recovery_c/examples",
            exclude: ["night_example.c"],
            sources: ["json.c"],
            publicHeadersPath: ".",
            cSettings: [.unsafeFlags(cFlags)]
        ),
        .executableTarget(
            name: "night_example",
            dependencies: ["recovery_c", "recovery_json"],
            path: "recovery_c/examples",
            exclude: ["json.c", "json.h"],
            sources: ["night_example.c"],
            cSettings: [.unsafeFlags(cFlags)],
            linkerSettings: [.linkedLibrary("m")]
        ),
        .executableTarget(
            name: "test_recovery",
            dependencies: ["recovery_c"],
            path: "recovery_c/tests",
            exclude: ["test_parity.c", "data"],
            sources: ["test_recovery.c", "synth.c"],
            cSettings: [
                .headerSearchPath("../include"),
                .headerSearchPath("../src"),
                .unsafeFlags(cFlags),
            ],
            linkerSettings: [.linkedLibrary("m")]
        ),
        .executableTarget(
            name: "test_parity",
            dependencies: ["recovery_c", "recovery_json"],
            path: "recovery_c/tests",
            exclude: ["test_recovery.c", "synth.c", "synth.h"],
            sources: ["test_parity.c"],
            resources: [.copy("data")],
            cSettings: [
                .headerSearchPath("../include"),
                .headerSearchPath("../examples"),
                .unsafeFlags(cFlags),
            ],
            linkerSettings: [.linkedLibrary("m")]
        ),
    ],
    cLanguageStandard: .c99
)

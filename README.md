

## Xcode（macOS）

用 Xcode 打开仓库根目录中的 `Package.swift`。工程是 C99 命令行库，不是带界面的 iOS/macOS App。

- 选择 `test_recovery` scheme、目标 `My Mac`，点 Run 可执行单元测试（无需运行参数）。
- 选择 `night_example` scheme；在 **Edit Scheme → Run → Arguments Passed On Launch** 添加：
  `$(PROJECT_DIR)/recovery_v1/examples/night_input_example.json`
- 选择 `test_parity` scheme；在同一处添加：
  `$(PROJECT_DIR)/recovery_c/tests/data/parity_cases.json`

这两个测试程序是独立命令行程序，不是 XCTest。若 Xcode 未展开 `$(PROJECT_DIR)`，请改用该文件的绝对路径。

## 命令行

在 `recovery_c/` 目录运行 `make test` 可执行单元测试和 parity 比对；该比对使用仓库中已导出的 JSON 用例，无需 Python。`make compare-example` 和 `make parity-data` 则需要仓库中的 `recovery_v1/` 参考实现。

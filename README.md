# FCA — Factorized Cyclotomic Authentication

Official C++20 implementation accompanying *FCA: Private Cloud Storage Auditing with Factorized Cyclotomic Authentication* by Zuhui Wan, Chunli Lv, and Xinjin Ge. It includes a local auditing CLI, correctness tests, and benchmarks for FCA and the dense and polynomial baselines.

本实现通过本地文件在数据所有者和存储服务器两个角色之间交换审计报文。

## 构建与测试

依赖 C++20 编译器、Make、OpenSSL 3 开发库和 Python 3.9 或更高版本。Makefile 会检测常见的 Homebrew OpenSSL 路径。其他非系统路径可通过 `OPENSSL_PREFIX=/path/to/openssl` 指定。

```sh
make -j
make test
make test-portable
```

默认使用 `-march=native`。支持的 ARM64 和 x86-64 机器使用 PMULL 或 PCLMUL 进行无进位乘法。可用 `make BUILD=build/portable ARCH_FLAGS=-DFCA_PORTABLE` 仅使用可移植实现。测试以独立的逐位多项式长除法核对优化算法，并穷举 `GF(4)` 中秩 1、秩 2、1 分支和 2 分支的认证输出分布。命令行测试还覆盖损坏数据、损坏标签、轮次重放、填充和畸形响应。

## 本地审计一份文件

```sh
tmp=$(mktemp -d)
mkdir "$tmp/owner"
printf 'local FCA demo\n' > "$tmp/input"
build/fca keygen "$tmp/owner/master.key"
build/fca prepare "$tmp/owner/master.key" "$tmp/input" "$tmp/server" "$tmp/owner/file.meta"
build/fca challenge "$tmp/owner/master.key" "$tmp/owner/file.meta" 1 "$tmp/request.bin"
build/fca prove "$tmp/server" "$tmp/request.bin" "$tmp/response.bin"
build/fca verify "$tmp/owner/master.key" "$tmp/owner/file.meta" "$tmp/request.bin" "$tmp/response.bin"
```

最后一条命令输出 `ACCEPT` 或 `REJECT`，退出码分别为 0 或 1，命令或本地配置错误为 2。所有者保存密钥、可信元数据及本轮发出的原始请求。每轮使用新的轮次号和随机种子。服务器只读取 `public.bin`、`blocks.bin`、`tags.bin` 与收到的请求。

- 默认 `p=179, ℓ=366, w=64, t=1`，每块 521,184 B，末块补零。空文件表示为一个全零块。默认抽取 `min(688, m)` 个块，`--samples C` 可在准备文件时指定抽样数。
- 两个快照用同一个主密钥和 `--scope $(build/fca scope)` 生成的同一 scope 值，分别保存到新的目录和元数据文件。不同 FID 的文件掩码独立派生。C++ 库允许跨版本共享同一个内存中的 `Authenticator`，单独运行的 CLI 进程会从根密钥重新派生同一组因子。
- 有限域支持满足 `ord_p(2)=p−1` 且 `p ≤ 251` 的素数。`prepare` 支持 `--prime P --columns L --width W --branches T`，文件模式的字宽须为 8 的倍数，内存 API 同时支持 1–64 位字宽。
- 认证和审计流程见论文第三节。线性基线使用各自独立派生的密集系数或 Horner 求值，共用掩码和聚合流程。
- 参数与记录采用小端编码，字段使用 `ceil((p−1)/8)` 字节，禁止非零填充位。块按列、再按行编码，每个字内部按小端放置。请求为 `FID[16] || round[8] || seed[32]`，响应为 `FID[16] || round[8] || aggregate_block || aggregate_tag`。HMAC-SHA256 的域标签、索引基数和报文编码为本项目的具体化。

这是静态文件完整性实验。抽样命中发生变更的块、聚合偏差未抵消且认证值未发生碰撞时，审计会发现局部变更。实现使用按因子置位枚举的卷积，适用范围是这里的本地复现实验。

## 重跑基准

```sh
tmp=$(mktemp -d)
build/fca-bench "$tmp/default.bin" 2061 100 16 688 > results/ablation.csv
build/fca-bench "$tmp/scaling.bin" 2061 10 16 128 256 512 1024 2048 2061 > results/scaling.csv
python3 scripts/analyze.py > results/analysis.json
```

`fca-bench` 依次接受新合成文件路径、总块数、每个抽样大小的轮数、每轮标签计时的块数及一个或多个抽样大小。两条完整基准各产生约 1 GiB 本地临时数据。基准专用的随机输入、密钥和种子固定以便重跑。详细计时边界和早期复现结果见 [实验报告](results/REPORT.md)。

## License

This implementation is available under the [MIT License](LICENSE).

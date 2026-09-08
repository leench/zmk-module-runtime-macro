# OMV 交付文件

## 当前交付

本次交付面向 Totem dongle 实机测试，文件放在 OMV 的：

```text
~/zmk-build/
```

| 文件 | 来源 | SHA256 |
| --- | --- | --- |
| `zmk.uf2` | `/home/niri/zmk_projects/zmk/app/build/totem_dongle/zephyr/zmk.uf2` | `f5e06d23e6f09ce256cbfcdfbff3c161a452bbb73e7cb62b8daeec4f97cd0536` |
| `runtime_macro_cli.py` | `tools/runtime_macro_cli.py` | `30643532f7ae1d30bbea19fd89188dd2bb9af472901e2ee90989f165008c3bcd` |

同名文件以本次交付覆盖。`zmk.uf2` 使用正式 `just totem-dongle` 构建，启用
keep-after-execute 协议扩展；当前 Totem RAM 使用 `194190 / 262144 B`。

## OMV 上的测试

在 OMV 上安装 `hidapi` 后，使用 Python client 探测能力或上传 dynamic macro：

```sh
cd ~/zmk-build
python3 -m pip install --user hidapi
python3 runtime_macro_cli.py capabilities
python3 runtime_macro_cli.py dynamic-set \
  --text 'KEEP TEST 123' \
  --keep-after-execute
```

默认上传仍然是执行后消费；`--keep-after-execute` 会在 TTL 到期、明确 clear、USB
disconnect、可选 lifecycle clear 或重新上传之前保留动态文本。

动态通道只用于个人使用约束下的非-secret 文本；不要上传密码、OTP、token、密钥或其他秘密。

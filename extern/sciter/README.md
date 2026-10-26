# Sciter SDK

Copy your Sciter SDK here:
- `include/` - Sciter headers (sciter-x.h, sciter-x-window.hpp, etc.)
- `lib/` - Sciter libraries (.lib files)
- `bin/` - (optional) sciter.dll for runtime

## Required Files
```
extern/sciter/
├── include/
│   ├── sciter-x.h
│   ├── sciter-x-window.hpp
│   └── ... (other headers)
└── lib/
    └── sciter.lib
```

## Vendored binaries (current: 6.0.5.4)

Both files are copied unmodified from the official SDK
<https://gitlab.com/sciter-engine/sciter-js-sdk>, release commit `d5f5fec4bc`
("6.0.5.4"). VKey ships the **Direct2D** build (`bin/windows.d2d`), not the
default Skia build in `bin/windows/x64`, so its hash differs from that file.

| File | Upstream path | SHA-256 |
|---|---|---|
| `bin/sciter.dll` | `bin/windows.d2d/x64/sciter.dll` | `da5355a85e94d802d5d482029dea208ac1743aae5c8b8b59ba9507135401bbb2` |
| `bin/packfolder.exe` | `bin/windows/packfolder.exe` | `1fc129c5dd3ea614897e20234135e084b1699953ef7aa2c9322dfe44b9c74a60` |

When bumping the SDK, update `VERSION`, `include/`, both binaries and this table
together.

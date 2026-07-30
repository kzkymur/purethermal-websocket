# purethermal-capture

PureThermal + FLIR Lepton の16bit Y16フレームを、CLIからraw動画とRGB AVIへ同時録画する
小さなツールです。WebSocketサーバーは使用しません。Windows、macOS、Linuxで
ダミー入力をビルドでき、libuvcとLepton SDKが揃っている場合は実機入力も有効になります。

出力はヘッダーなしのlittle-endian `uint16` フレーム列（`gray16le`）です。温度情報を
8bitへ落とさず保存します。録画中に解像度が変わった場合は、壊れたraw動画にしないため
録画を停止します。

同時に、指定温度範囲を青→水色→黄→赤へ変換した、非圧縮24bit RGB AVIを出力します。
たとえば`-o thermal.y16`なら、既定の動画名は`thermal.avi`です。

## Windowsでビルド

Visual Studio 2022の「C++によるデスクトップ開発」、CMake、Gitを用意してください。

WindowsではOS標準のMedia FoundationからPureThermalのY16ストリームを取得するため、
libuvcは不要です。接続したカメラがデバイスマネージャーの「カメラ」に
`PureThermal`として表示される状態で使用してください。

```powershell
cmake -S . -B build -DENABLE_LIBUVC=OFF
cmake --build build --config Release
.\build\Release\lepton_capture.exe --mode pt3 --duration 10 -o thermal.y16
```

ダミー入力での確認:

```powershell
.\build\Release\lepton_capture.exe --mode dummy --duration 10 -o capture.y16
```

## macOS / Linuxでビルド

libuvcが不要な確認用ビルド:

```sh
cmake -S . -B build -DENABLE_LIBUVC=OFF
cmake --build build -j
./build/lepton_capture --mode dummy --duration 10 -o capture.y16
```

実機を使う場合はlibuvc/libusbを導入し、サブモジュールを初期化してから通常どおり
CMakeを実行してください。

## CLI

```text
lepton_capture [--mode dummy|pt3] [-o FILE]
               [--duration SEC | --frames NUM]
               [--fps auto|NUM] [--scale NUM]
               [--video-output FILE]
               [--video-min-c NUM] [--video-max-c NUM]
               [--assume-tlinear|--no-assume-tlinear]
               [--ffc-mode manual|auto|external]
```

- `--mode`: `dummy`または`pt3`（既定: `dummy`）
- `-o`, `--output`: 出力先（既定: `capture.y16`）
- `--duration`: 録画秒数（既定: `10`、`0`ならCtrl+Cまで）
- `--frames`: 指定フレーム数で停止。指定時は`--duration`より優先
- `--video-output`: RGB AVIの出力先（既定: Y16と同名の`.avi`）
- `--video-min-c`: RGB表示の最低温度（既定: `20`℃）
- `--video-max-c`: RGB表示の最高温度（既定: `40`℃）
- `--fps`: `auto`または数値（既定: `auto`）
- `--scale`: TLinearのKelvin倍率（既定: `100`）
- `--ffc-mode`: 実機のFFCモード（既定: `manual`）

例:

```powershell
# 100フレーム録画
.\build\Release\lepton_capture.exe --mode pt3 --frames 100 -o thermal.y16

# Ctrl+Cまで録画
.\build\Release\lepton_capture.exe --mode pt3 --duration 0 -o thermal.y16
```

上記はいずれも`thermal.y16`と`thermal.avi`の2ファイルを作成します。表示範囲を
変える場合:

```powershell
.\build\Release\lepton_capture.exe --mode pt3 --duration 10 `
  -o thermal.y16 --video-min-c 15 --video-max-c 50
```

## 再生・変換

Lepton 3（160x120）、9 fpsの例:

```sh
ffplay -f rawvideo -pixel_format gray16le -video_size 160x120 -framerate 9 thermal.y16
ffmpeg -f rawvideo -pixel_format gray16le -video_size 160x120 -framerate 9 \
  -i thermal.y16 -vf normalize -pix_fmt yuv420p thermal.mp4
```

raw形式には解像度・fps・温度scaleのメタデータは入りません。録画完了ログに表示される
解像度を、再生・変換時に指定してください。TLinearの場合は各画素値を`--scale`で割ると
Kelvinになります。

既定のTLinear設定では、Y16の各画素から次の式で温度を復元できます。

```text
Kelvin = uint16_le / scale
Celsius = uint16_le / scale - 273.15
```

既定の`scale`は100です。Y16自体にはscaleや解像度が格納されないため、解析時には録画時の
`--scale`と解像度を一緒に管理してください。RGB AVIは表示用で、量子化・色変換済みのため
正確な温度復元には使用できません。

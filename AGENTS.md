# AGENTS.md（uvc-y16-websocket）

このファイルは、本リポジトリ（`uvc-y16-websocket`）でエージェント／開発者が作業する際の合意事項をまとめたものです。ここに記載のルールは、このファイルが置かれているディレクトリ配下のすべてのファイルに適用されます（より深い階層に別の AGENTS.md があれば、そちらが優先されます）。

## 目的
- UVC デバイス（例: PureThermal + FLIR Lepton）から取得する 16bit グレースケール（Y16）フレームを WebSocket で配信する最小構成のサーバーを維持・発展させる。
- 既存のバイナリ・ワイヤプロトコルを壊さず、安定して運用できる状態を保つ。

## ビルドと実行
- 要件
  - CMake ≥ 3.20 / C++20 対応コンパイラ
  - Boost（ヘッダのみで可）
  - libuvc（および libusb-1.0）
- 例（ビルド）
  - `cmake -S . -B build && cmake --build build -j`
  - あるいは README の簡易コマンド `cmake . && make`
- 例（実行）
  - 実機: `sudo ./lepton_ws_server --mode pt3 --port 8765 --fps 9`
  - ダミー: `./lepton_ws_server --mode dummy --port 8765 --fps 9`
- macOS(Homebrew) 例: `brew install boost libuvc libusb`

## コーディング規約（C++）
- 標準: C++20。外部依存は最小限。
- 名前付け
  - 型・クラス: `PascalCase`（例: `SourceMonitor`）
  - 関数・変数: `snake_case`（例: `parse_args`）
  - 定数: 接頭辞 `k` + `CamelCase`（例: `kFormatUint16TLinear`）
- スタイル
  - インクルードは標準→外部→自前の順。未使用のインクルードを置かない。
  - ログは `log(LogLevel, msg)` を使い、I/O 例外や終了を伴う強い動作は避ける。
  - 共有データは必ずミューテックスで保護。ブロッキングしやすい処理をロック内に長く置かない。
- 構成
  - 現状は単一 TU（`main.cpp`）。分割が必要な場合は、ヘッダ（`.hpp`）+ 実装（`.cpp`）を追加し、CMake を最小変更で拡張する。

## 変更方針（重要）
- ワイヤプロトコル互換性を最優先。
- パフォーマンスとリアルタイム性を阻害する不要な動的確保・コピーを避ける。
- 例外の使用は引数解析など限定的な場面にとどめ、実行時は戻り値とログ中心で扱う。
- 依存追加は慎重に。巨大フレームワークや生成ツールを持ち込まない。

## ワイヤプロトコル（固定 32 バイトヘッダ + ピクセル）
- ヘッダ構造体 `FrameHeader` は「常に 32 バイト」。フィールドの並び・意味を変更しない。
  - `magic = "L3R1"`、`version = 1`、`format = 1 (UINT16_TLINEAR)`、`scale = 100`（値/100 が Kelvin）。
  - `timestamp_us` はモノトニック（単調増加）由来、`frame_id` は単調増加。
  - ピクセルはリトルエンディアン `uint16_t` を `width * height` 個。
- 破壊的変更が必要な場合
  - `version` をインクリメントし、旧版との後方互換（受信側の読み分け）を用意してから適用する。
  - README とクライアント向けサンプルの更新を同時に行う。

## WebSocket の振る舞い
- サーバーは「バイナリフレームのみ」を送信。クライアントからのメッセージは現在未使用（将来の制御用に予約）。
- `Session` の送信キュー上限は既定で 2。詰まりが発生した場合は古いフレームを捨て「最新優先」を維持する（この方針を変えない）。

## デバイスソース設計
- 取得源は `IFrameSource` 抽象に従う。
  - 現在: `DummySource`（合成）/ `PT3Source`（libuvc 実機）を `SourceMonitor` で監視・再接続。
- 新しいソースを追加する場合
  - 既存インターフェイスを守る（`start/stop/latest/width/height`）。
  - ビルドの有無を `#ifdef` で切り分け、ダミーモードは常にビルド可能に保つ。

## CMake 方針
- Boost はヘッダのみを前提にし、不要なリンクを増やさない（`Boost::headers` があれば利用）。
- libuvc は `pkg-config` もしくは Homebrew パスで探索。検出ロジックを壊さない。
- 条件付き機能は `USE_LIBUVC` 等の定義で切り替える。テスト時は定義の有無双方を意識したコードにする（CMake の既定は尊重）。

## テスト / 動作確認のヒント
- ダミーモードでの疎通確認
  - サーバー: `./lepton_ws_server --mode dummy --port 8765`
  - クライアント例（Python, 簡易検証）
    ```python
    import asyncio, struct, websockets
    async def main():
        async with websockets.connect("ws://127.0.0.1:8765") as ws:
            buf = await ws.recv()  # 1 フレーム
            magic, ver, hbytes, w, h, fmt, scale, _, ts, fid, _ = struct.unpack('<4sHHHHHHHQLH', buf[:32])
            assert magic == b'L3R1' and hbytes == 32
            print('w,h,fmt,scale,fid=', w, h, fmt, scale, fid)
    asyncio.run(main())
    ```
- 実機モード（`--mode pt3`）では USB アクセス権のために `sudo` が必要な場合があります。

## レビュー / コミット規約
- コミットメッセージは Conventional Commits を推奨
  - 例: `feat(ws): add optional heartbeat frame`
  - 種別: `feat` `fix` `perf` `refactor` `docs` `test` `build` `chore`
- 変更が次に該当する場合は README を同時更新
  - ビルド要件・実行方法・プロトコル仕様・引数一覧。

### 作業完了前チェック（必須）
- ローカルでビルドが通ることを確認する。
  - `cmake . & make`（README の簡易例に準拠）
  - 注: 一般的には逐次実行の `cmake . && make` を推奨（`&` は並列起動のため環境により不安定）。
  - もしくは `cmake -S . -B build && cmake --build build -j`
- 実機依存の変更であっても、少なくとも `--mode dummy` での起動可否は確認する。

## 禁止事項 / 注意点
- 既存のヘッダ・プロトコルを暗黙に変更しない（互換プランなしの改変禁止）。
- 大規模依存の追加やジェネレータ導入を安易に行わない。
- ライセンス文言の書き換え禁止。著作権ヘッダの一括挿入もしない。

## よくある作業の指針
- 速度低下や途切れの報告がある場合
  - まず送信キュー詰まりの有無と `broadcast` 周期を確認。
  - `SourceMonitor` の stall 検出と再接続ログを確認。
- 解像度や FPS の変更
  - 取得元の設定とヘッダ値の一貫性を保つ（`width/height/scale`）。
  - 受信側が期待する `uint16 LE` を崩さない。

---
この AGENTS.md に反する変更が必要な場合は、根拠と影響範囲を PR 説明に明記してください。最低限、互換性・可用性・保守性への配慮を示すこと。

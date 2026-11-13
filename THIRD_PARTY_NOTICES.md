# 第三者コードと配布上の注意

## TVTest SDK

- 対象: `TVTestPlugin.h`、`TvRockOnTVTest.cpp` 内の SDK 互換定義
- 原著作者表記: DBCTRADO
- 出典: https://github.com/DBCTRADO/TVTest/blob/develop/sdk/TVTestPlugin.h
- 利用条件: CC0 1.0 Universal
- ライセンス本文: `licenses/CC0-1.0.txt`
- 本リポジトリでの変更: UTF-8 BOM・改行の整備、旧 SDK の構造体レイアウトを維持するバージョン条件の調整、サンプル内の人名を汎用表記に変更。プラグイン側には必要なステータス項目の互換宣言を含みます。

TVTest 本体はプラグインのリリース ZIP に含みません。SDK ヘッダの CC0 と TVTest 本体のライセンスは別です。

## TSTask の差し替えソース

- 対象: `TSTask/TvRock.cpp`、`TSTask/TvRock.h`
- 出典: https://github.com/DBCTRADO/TSTask
- 原著作者: TSTask の原著作者・貢献者（DBCTRADO）
- 利用条件: GPL v2（上流 README の表記）
- ライセンス本文: `licenses/GPL-2.0.txt`
- 本リポジトリでの変更: DLL の読み込み・ABI 連携、選局・初期チャンネル選択、リモートストリーム処理などの変更を含みます。原版と同一ではありません。

このリポジトリは差し替えソースのみを提供します。TSTask 本体・アイコン・実行ファイルはプラグインのリリース ZIP に含みません。これらのソースを組み込んだ実行ファイルを配布する場合は、組み込み先全体について GPL の条件に従い、対応するソースと必要な通知を提供してください。

## リリース ZIP

`build-artifact.ps1` は x64 / x86 の TVTest プラグインに加えて、`README.md`、`LICENSE.md`、本書、`licenses/` の文書を配布フォルダへコピーします。GitHub / GitLab の公開処理はそのフォルダ全体を ZIP にするため、これらの文書も収録されます。GPL の対象である RecTest / TSTask の差し替えソースはソースリポジトリに含まれ、プラグインのバイナリには組み込まれません。

# CMake の最低バージョン
# サンプルに従って設定しただけで2.8の必要性は確認していない
cmake_minimum_required(VERSION 2.8)

# メイク時の詳細な表示を有効にする
set(CMAKE_VERBOSE_MAKEFILE 1)

# リリースビルド用オプション (CXXはC++)
set(CMAKE_CXX_FLAGS_RELEASE "-Wall -O2 -DNDEBUG")

# デバッグビルド用オプション
set(CMAKE_CXX_FLAGS_DEBUG "-Wall -g -O0 -DDEBUG")

# ビルドタイプはリリース
# デバッグビルドはコマンドラインから以下を実行
# cmake -DCMAKE_BUILD_TYPE=Debug ..
set(CMAKE_BUILD_TYPE Release)

# 共有ライブラリのプリフィックス(通常"lib")を省略する
set(CMAKE_SHARED_LIBRARY_PREFIX_CXX "")

# プロジェクト名
project(BonDriver_Popen CXX)

# コンパイルするファイル一覧 src/*.cpp を変数 srcs に追加
file(GLOB srcs src/*.cpp)

# 共有(SHARED)ライブラリを変数 srcs のファイルから作成する
add_library(BonDriver_Popen SHARED ${srcs})

# リンクする外部ライブラリ
target_link_libraries(BonDriver_Popen pthread)
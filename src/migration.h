// src/migration.h
// [Schema Migration] APO GUID / 레지스트리 구조가 바뀔 때 idempotent 청소.
// 한 번 들어간 옛 GUID 는 kHistoricalApoGuids 에서 절대 제거하지 않는다 —
// 사용자가 어느 옛 버전에서 올라오든 깨끗하게 정리.
//
// 호출 흐름:
//   1) setup main() 시작 직후 RunSchemaMigration() — 업그레이드 시 옛 잔재 strip
//   2) 기존 Step 1~7 (DLL 복사 / APO 등록 / 기본 디바이스 처리)
//   3) main() 끝 직전 WriteSchemaVersion() — HKLM\SOFTWARE\SoundMate\SchemaVersion 작성

#pragma once
#include <windows.h>
#include <string>

// 현재 스키마 버전. APO GUID / 레지스트리 구조가 바뀌면 +1 한다.
inline constexpr DWORD CURRENT_SCHEMA_VERSION = 1;

// 옛 GUID 목록. 새 GUID 를 도입하면 옛 GUID 를 여기에 append. nullptr 종결.
// 절대 제거 금지 — 모든 새 setup 이 이 리스트를 항상 strip 한다.
extern const wchar_t* kHistoricalApoGuids[];

// 현재 SoundMate GUID + kHistoricalApoGuids 전체와 비교.
// 옛 our-GUID 가 PreMixChild 백업에 끼어 들어 circular chain 사고 나는 것을 방지.
bool IsAnyOurGuid(const std::wstring& g);

// 업그레이드 시 옛 스키마 잔재 제거. 신규 설치(HKLM\SOFTWARE\SoundMate 부재) 면 no-op.
void RunSchemaMigration();

// 설치 마지막에 CURRENT_SCHEMA_VERSION 작성. 다음 설치가 idempotent 분기 가능.
void WriteSchemaVersion();

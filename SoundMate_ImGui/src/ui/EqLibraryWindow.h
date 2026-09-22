// src/ui/EqLibraryWindow.h
// [v0.1.1] "EQ 관리" 창.
//
// song_cache.json 에 쌓인 곡별 EQ 를 목록으로 보여 주고, 곡 단위로 지울 수
// 있게 한다. 지금까지는 캐시를 통째로 비우는 길밖에 없어서, 한 곡만 잘못
// 학습됐을 때도 전체를 날려야 했다.
//
// 데이터는 열 때 한 번만 읽어 스냅샷으로 들고 있는다. 곡 해석 스레드가
// 배경에서 계속 캐시에 쓰기 때문에 매 프레임 읽으면 목록이 흔들리고,
// RecordManager 의 뮤텍스를 프레임마다 잡게 된다.
#pragma once
#include "imgui.h"

#include "../core/RecordManager.h"

#include <string>
#include <vector>

class EqLibraryWindow {
public:
  void Open();
  void Render();
  bool IsOpen() const { return m_open; }

private:
  void Reload();
  bool PassesFilter(const RecordManager::CachedSongInfo &s) const;

  bool m_open = false;
  std::vector<RecordManager::CachedSongInfo> m_songs;

  char m_filter[128] = {0};

  // 삭제 확인 대상. 비어 있으면 확인창이 떠 있지 않다는 뜻이다.
  std::string m_pendingDeleteKey;
  std::string m_pendingDeleteLabel;
  bool m_confirmClearAll = false;

  // 마지막 동작 결과 한 줄. 지웠는지 아닌지가 화면에 남아야 사용자가
  // 버튼이 먹었는지 확인할 수 있다.
  std::string m_status;
};

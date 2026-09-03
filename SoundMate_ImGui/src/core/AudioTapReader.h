// src/core/AudioTapReader.h
//
// APO(audiodg)가 링버퍼에 흘려주는 EQ 적용 **전** 모노 오디오를 읽어오는 소비자.
// 레이아웃 정의는 엔진 쪽 SoundMate_AudioTap.h 를 공유한다 (SSOT).
//
// 사용 규약:
//   Open()        : 섹션이 아직 없으면 실패한다 — APO 가 LockForProcess 에서
//                   만들기 때문. 실패는 정상 상태이므로 주기적으로 재시도할 것.
//   SetActive(true): "지금 듣고 있다" 를 알린다. 이걸 켜야 APO 가 복사를 시작한다.
//                   끄면 audiodg 는 단 한 바이트도 복사하지 않는다.
//   Heartbeat()   : 최소 1초에 한 번은 호출. 끊기면 APO 가 기록을 멈춘다
//                   (UI 가 죽었는데 audiodg 만 계속 복사하는 것을 막는 장치).
//   Read()        : 새로 쌓인 프레임을 꺼낸다. 논블로킹.
#pragma once

#include <cstddef>
#include <cstdint>

class AudioTapReader {
public:
  AudioTapReader();
  ~AudioTapReader();

  AudioTapReader(const AudioTapReader&) = delete;
  AudioTapReader& operator=(const AudioTapReader&) = delete;

  // 이미 존재하는 섹션을 연다. 만들지는 않는다.
  bool Open();
  void Close();
  bool IsOpen() const { return m_view != nullptr; }

  // 섹션이 열려 있고 살아 있는 writer(APO 인스턴스)가 소유권을 쥐고 있는가.
  // false 면 재생 중이 아니거나 구버전 APO 가 로드돼 있다는 뜻.
  bool HasWriter() const;

  void SetActive(bool active);
  void Heartbeat();

  // 새 프레임을 out 으로 복사한다. 반환값 = 복사한 프레임 수.
  // outLostData: 링버퍼가 한 바퀴 돌아 데이터를 놓쳤으면 true (분석 리셋 신호).
  size_t Read(float* out, size_t maxOut, bool* outLostData);

  // writer 가 게시한 스트림 포맷. 아직 없으면 0.
  uint32_t SampleRate() const;
  uint32_t SourceChannels() const;

  // 다음 Read() 가 "지금 이 순간"부터 읽도록 읽기 위치를 최신으로 당긴다.
  // 곡이 바뀌어 이전 곡 오디오를 버려야 할 때 호출.
  void SkipToLatest();

private:
  void*    m_map;   // HANDLE
  void*    m_view;  // SoundMateAudioTap*
  uint64_t m_readIndex;
  bool     m_primed;  // 최초 Read 에서 읽기 위치를 맞췄는가
};

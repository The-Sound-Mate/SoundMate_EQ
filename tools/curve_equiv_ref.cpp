// tools/curve_equiv_ref.cpp
//
// 난독화 이전(HEAD)의 LocalCurve.cpp 를 RefCurve 네임스페이스로 컴파일한다.
// 새 구현과 같은 실행 파일에 넣고 전수 비교하기 위한 것이다 — 빌드 산출물에는
// 들어가지 않는 검증 전용 TU.
//
// 헤더 이름은 매크로 확장 대상이 아니므로 #include "LocalCurve.h" 는 그대로
// 동작하고, 그 안의 namespace LocalCurve 만 RefCurve 로 바뀐다.
#define LocalCurve RefCurve
#include "localcurve_ref.cpp"

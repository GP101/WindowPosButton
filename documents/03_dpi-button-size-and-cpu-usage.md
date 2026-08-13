# DPI 버튼 크기 불일치 및 CPU 점유율 개선

작업일: 2026-08-13
대상 파일: `WindowPosButton/WindowPosButton.cpp`

이 문서는 한 세션에서 진행한 두 가지 작업을 정리한다: (1) 서로 다른 DPI 배율의
모니터 사이로 창을 옮길 때 오버레이 버튼 크기가 일관되지 않는 문제, (2) 진단
로깅으로 인해 상시 발생하던 불필요한 CPU/디스크 I/O 부하.

## 1. DPI 경계 이동 시 버튼 크기 불일치

### 배경

WindowPosButton은 각 대상 창의 타이틀바 옆에 4개의 오버레이 버튼(스냅 좌/우,
센터 80%, 다음 모니터로 이동)을 별도의 `WS_POPUP` 레이어드 윈도우로 그린다.
사용자가 175%↔100%처럼 DPI 배율이 다른 모니터 사이로 창을 옮길 때 이 버튼들의
크기가 일관되게 유지되는지 확인해 달라는 요청을 받았고, 정적 분석 결과 실제로
재현 가능한 구조적 원인을 확인했다.

### 원인

버튼 크기를 계산하는 `ComputeButtonRect`(`WindowPosButton.cpp`)는 매번 서로 다른
서브시스템에서 아래 값을 비원자적으로(non-atomic) 순차 조회한다.

1. `DwmGetWindowAttribute(DWMWA_CAPTION_BUTTON_BOUNDS)` — DWM이 관리, 대상 창이
   재도장된 후에야 갱신
2. `GetWindowRect` — 창 외곽 사각형
3. `GetMonitorDpi`(`MonitorFromWindow` + `GetDpiForMonitor`) — "가장 가까운
   모니터"의 DPI. 경계에 걸치면 판정이 프레임마다 뒤바뀔 수 있음
4. `GetDpiForWindow` — 대상 창 자신의 DPI. 대상 앱의 DPI 인식 수준에 따라 다른
   타이밍에 갱신됨

`scale = monitorDpi / windowDpi`로 이 값들을 조합하는데, 드래그 중 이 값들이 서로
다른 순간을 가리키면(race) 버튼 크기가 순간적으로, 때로는 드래그가 끝난 뒤에도
어긋난 채 고정된다. `EVENT_OBJECT_LOCATIONCHANGE`가 드래그 중 연속으로
재계산(`DispatchUpdate` → `ComputeButtonRect`)을 트리거하고, 마지막 이벤트의
스냅샷이 최종 크기로 굳어지는 구조라 이 문제가 증폭됐다. `MoveWindowToNextMonitor`
("다음 모니터로 이동" 버튼)가 모니터 이동 시 창 크기를 DPI 비율로 재조정하지
않는 점도 이 경합을 더 쉽게 유발하는 부가 요인으로 확인됐다 (이번 수정 범위에는
포함하지 않음).

### 적용한 수정

- **`MonitorFromWindow` 중복 호출 제거**: `ComputeButtonRect`가 DPI 조회와
  작업 영역 클램프에 각각 별도로 `MonitorFromWindow`를 호출하던 것을, 한 번만
  조회한 `HMONITOR`를 재사용하도록 변경. 경계를 드래그하는 도중 두 호출 사이에
  "가장 가까운 모니터" 판정이 뒤바뀌어 DPI와 작업 영역이 서로 다른 모니터
  기준이 되는 경우를 줄인다.
- **정착 확인(settle-confirmation) 타이머 추가**: `DispatchUpdate`가 호출될
  때마다 창(HWND)별 원샷 타이머(`kOverlaySettleDelayMs = 150ms`)를
  재무장(re-arm)한다. `SetTimer`를 같은 ID로 다시 호출하면 카운트다운이
  리셋되므로, 이 타이머는 해당 창에 대한 **마지막** 이벤트로부터 150ms 뒤에 정확히
  한 번만 발화한다. 그 시점이면 DWM 캡션 영역 갱신과 대상 창의 DPI 변경 처리가
  대부분 끝나 있어, 드래그 도중 찍힌 어긋난 크기를 깨끗한 값으로 덮어쓰는 확인용
  재계산이 실행된다. 타이머 콜백 자신은 `rearmSettleTimer=false`로 호출해 무한
  재무장을 방지했다.
- 기존 "복원 중 재시도" 타이머(`ApplyUpdateResult`)와 동일한 per-window 타이머
  슬롯/`WM_TIMER` 처리 로직을 재사용해 변경 폭을 최소화했다.

### 한계

계산 자체의 4-값 비원자성을 완전히 없앤 것은 아니고, "정착 후 한 번 더
확인"하는 방식으로 최종적으로 보이는 크기를 안정화한 것이다. 실제
175%↔100% 모니터 간 드래그를 통한 육안 검증은 개발 환경에 해당 하드웨어가
없어 수행하지 못했다.

## 2. CPU 점유율 — 상시 활성화된 진단 로깅의 동기 파일 I/O

### 배경

CPU 점유율을 낮추는 것이 중요하다는 요청에 따라 코드를 다시 점검했다.

### 원인

- `InitializeDiagnosticLog()`가 `wWinMain`에서 조건 없이 호출되고, `_DEBUG`/
  `NDEBUG` 등의 조건부 컴파일이 전혀 없어 **Release(배포) 빌드에서도 로깅이
  항상 켜져 있었다.**
- `AppendDiagnosticLine`이 로그 한 줄을 쓸 때마다 파일을
  `_wfopen_s`로 열고 `fputs` 후 `fclose`로 닫았다 — 매번 CreateFile/CloseHandle
  쌍이 발생.
- `SetWinEventHook` 5개가 모두 `hProcess=0, idThread=0`, 즉 **데스크톱 전체
  (모든 프로세스)**를 대상으로 걸려 있었다. 그중 `EVENT_OBJECT_REORDER`는
  "데스크톱의 최상위 Z-order가 바뀔 때마다" 발생하는데(알트탭, 툴팁, 메뉴 열림
  등 이 앱과 무관한 이유로도 트리거됨), 이 이벤트만 스로틀링 없이 매번
  `LogWindowDiagnostic` → 파일 open/write/close를 실행했다. 이 모든 처리는
  오버레이 렌더링·타이머 처리와 동일한 메인 UI 스레드(`WINEVENT_OUTOFCONTEXT`)
  에서 동기적으로 실행된다.

즉 데스크톱을 평범하게 사용하기만 해도 이 앱과 무관한 이유로 초당 수십 번의
동기 디스크 I/O가 메인 스레드에서 발생할 수 있었다.

### 적용한 수정

- **로그 파일 핸들 상시 유지**: `g_diagnosticLogFile`을 앱 시작 시
  (`InitializeDiagnosticLog`) 한 번만 열고, 메시지 루프 종료 직후
  (`ShutdownDiagnosticLog`)에만 닫는다. `AppendDiagnosticLine`은 이제 열려있는
  핸들에 `fputs` + `fflush`만 수행해 매 줄마다 발생하던 CreateFile/CloseHandle
  왕복을 없앴다 (durability를 위해 `fflush`는 유지).
- **`EVENT_OBJECT_REORDER` 스로틀링**: 기존에 `EVENT_OBJECT_LOCATIONCHANGE`에
  적용되어 있던 것과 동일한 방식(1초에 요약 1줄)으로 `LogReorderEventThrottled`
  를 추가해 REORDER 로그도 스로틀링했다. REORDER의 `hwnd`는 특정 창이 아니라
  보통 데스크톱 컨테이너이므로, 창별이 아닌 전역 상태(`g_reorderEventLog`) 하나로
  충분하다.

### 검증

`MSBuild WindowPosButton.vcxproj /p:Configuration=Release /p:Platform=x64`로
정상 컴파일 확인. 실사용 중 CPU/디스크 사용량 감소 여부는 실제 배포 환경에서
확인이 필요하다.

## 변경되지 않은 항목 (참고)

- `MoveWindowToNextMonitor`가 모니터 이동 시 창 크기를 소스/대상 DPI 비율로
  재조정하지 않는 문제는 이번 작업 범위에 포함하지 않았다.
- `MeasureThreadProc`이 디스패치마다 `CreateThread`로 새 스레드를 생성하는
  부분(코얼레싱으로 폭주는 억제되어 있음)과, `g_locationEventLogs` 맵이 추적
  대상이 아닌 창의 항목까지 계속 쌓이는 부분(메모리 이슈, CPU 이슈는 아님)은
  이번 작업에서 우선순위가 낮다고 판단해 보류했다.

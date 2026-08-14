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
않는 점도 이 경합을 더 쉽게 유발하는 부가 요인으로 확인됐다 (후속 수정, 3절 참고).

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

## 3. 후속 조치 — 다음 모니터 이동 시 DPI 재조정, 스레드 생성 방식 최적화

CPU 점유율을 절대 늘리지 않아야 한다는 요구에 따라, 1절에서 부가 요인으로만
지목했던 `MoveWindowToNextMonitor`와, 2절 이후에도 남아 있던 `CreateThread`
방식을 마저 손봤다.

### 3-1. `MoveWindowToNextMonitor` — 모니터 작업 영역 비율(%) 유지

기존에는 창을 다음 모니터로 옮길 때 픽셀 크기를 목적지 작업 영역에 맞춰
**클램프만** 했다. 처음에는 이를 소스/대상 모니터의 DPI 비율(`dpiScale =
destinationDpi / sourceDpi`)로 재조정하도록 고쳤으나, 사용자가 실제로 원한 동작은
DPI 비율이 아니라 **모니터 작업 영역에 대한 백분율(%) 그대로 유지**였다 — 예:
"현재 모니터에서 왼쪽에 붙어서 50%를 차지했다면, 이동한 모니터에서도 왼쪽에
붙어서 50%가 되어야 한다." DPI 비율과 모니터 해상도(작업 영역 크기) 비율은
일반적으로 서로 다른 값이므로 DPI 기반 접근으로는 이 요구를 정확히 만족시키지
못해, 아래와 같이 다시 고쳤다.

`GetVisibleFrameRect`로 구한 소스 창 사각형에서
`leftRatio`/`topRatio`/`widthRatio`/`heightRatio`(모두 소스 모니터 작업 영역
대비 비율, `[0, 1]`로 클램프)를 계산한 뒤, 이 비율들을 그대로 대상 모니터
작업 영역에 적용한다 (`destination.work.left + leftRatio * destinationWidth`
등). 이 앱의 다른 배치 버튼(`PerformButtonAction`의 SnapLeft/SnapRight/
Center80, `targetWidth = monWidth * widthRatio`)이 이미 이 방식으로 동작하고
있어, `MoveWindowToNextMonitor`도 같은 방식으로 통일한 것이다. 이제 사용하지
않게 된 `GetMonitorDpi(source->monitor)`/`GetMonitorDpi(destination.monitor)`
호출과 `ScaleWindowPosition`("남는 여백 비율 보존" 방식의 옛 위치 계산
함수, 유일한 두 호출부가 모두 교체되어 사용처가 없어짐)은 제거했다.

최대화 상태 보존은 별도 손질 없이 그대로 성립한다: 최대화 상태였던 창의
`sourceRect`는 이미 소스 모니터의 거의 전체(~100%)이므로, 위 비율 공식을 그대로
적용해도 대상 모니터의 거의 전체(~100%)로 계산되고, 기존 `if (wasMaximized)`
분기(복원 위치로 이동 → `ShowWindow(SW_MAXIMIZE)`로 재최대화)가 대상 모니터에서
정확히 재최대화한다.

#### 3-1-1. 실기기 테스트에서 발견된 추가 결함 — 배치 직후 크기가 다시 바뀜

실제 175%(왼쪽)/100%(오른쪽) 듀얼 모니터에서 테스트한 결과, 위 비율 계산 자체는
정확했지만(사용자 확인: "이동한 위치와 크기는 정확한 것 같다"), **배치 직후
크기가 한 번 더 바뀌는** 문제가 관찰됐다. 원인은 우리 코드가 아니라 Windows의
DPI 가상화다: 창이 DPI가 다른 모니터로 넘어가면 Windows가 대상 창에
`WM_DPICHANGED`를 보내고, DPI 인식 앱은 보통 이 메시지에 반응해 스스로
크기/위치를 재조정한다 — 이때 앱이 쓰는 스케일링 기준(보통 DPI 비율 기반)이
우리가 방금 적용한 "모니터 작업 영역 %" 기준과 달라, 우리가 배치한 직후에
창이 다시 어긋난 크기로 바뀌는 것으로 보인다. 이는 대상 프로세스 자체의 메시지
처리이므로 WindowPosButton이 직접 막을 수는 없다.

**1차 시도 — "마지막에 다시 덮어쓰기"(실패)**: `MoveWindowToNextMonitor`가
비최대화 창을 배치한 뒤 목표 사각형을 기억해뒀다가, `kMonitorMoveReassertDelayMs
= 300ms` 뒤 같은 사각형을 한 번 더 적용하는 방식으로 처음 구현했다. 실기기로
확인해 보니 "정확히 배치됨 → (300ms 동안) 잘못된 크기로 유지 → 300ms 뒤 다시
정확한 크기로 튐"처럼 **두 번의 뚜렷한 크기 변화**로 보여 시각적으로 좋지
않았다. `EVENT_OBJECT_LOCATIONCHANGE` 훅에서 즉시(다음 프레임 수준으로) 재적용
하도록 반응 속도를 높이는 것도 시도했지만, 사용자가 재확인한 결과 여전히 두 번
바뀌는 것처럼 보였다 — 대상 앱이 자기 방식대로 잘못된 크기로 렌더링하는 순간이
아무리 짧아도 화면에 한 프레임이라도 노출되면 사람 눈에는 "두 번 조정"으로
읽히기 때문이다. Microsoft의 공식 DPI 인식 가이드도 "`WM_DPICHANGED`의
`lParam`이 제안하는 사각형을 그대로 적용하라"고 안내하므로, 잘 만들어진
DPI 인식 앱일수록 오히려 이 문제를 더 확실히 일으킨다 — 즉 아무리 빨리
반응해도 근본적으로 한 번의 "튀는" 프레임을 완전히 없앨 수는 없었다.

**2차 시도 — "숨겼다가 최종 상태로만 보여주기"(채택)**: 대신, 비최대화 창을
배치하기 직전에 `ShowWindow(hwnd, SW_HIDE)`로 숨긴 뒤 목표 사각형을 적용한다.
숨겨진 상태에서는 대상 앱이 `WM_DPICHANGED`에 반응해 스스로 크기를 몇 번을
바꾸든 DWM이 그 창을 합성(렌더링)하지 않으므로 화면에 전혀 노출되지 않는다.
`kMonitorMoveReassertDelayMs`(300ms) 뒤 `RevealMonitorMovePlacementIfPending`가
호출되어, 창이 아직 그 자리에 있으면(최소화/최대화되지 않았으면)
`SetWindowPosForVisibleFrame`로 목표 사각형을 한 번 더 적용해 대상 앱이 숨어
있는 동안 바꿔놓았을 수 있는 값을 덮어쓴 다음, `ShowWindow(SW_SHOW)` +
`SetForegroundWindow`로 그제서야 화면에 드러낸다. 사용자에게는 버튼을 누르고
약간의(300ms) 지연 후 창이 정확한 위치/크기로 "한 번만" 나타나는 것으로
보인다 — 중간 과정이 전혀 보이지 않으므로 두 번 조정되는 문제가 근본적으로
사라진다.

이 접근에서는 사각형을 실제로 비교해 "정말 바뀌었을 때만" 재적용하는 이벤트
기반 경로가 더 이상 필요 없어 제거했다(숨겨진 동안은 몇 번을 재적용해도 아무
것도 보이지 않으므로, 빠르게 반응할 이유가 없다). 대신 `MoveWindowToNextMonitor`
가 숨긴 직후 `DispatchUpdate(hwnd)`를 호출하지 않도록 주의했다 — 창이 아직
숨겨져 있으면 `ComputeUpdatePlan`이 "준비 안 됨"으로 보고, `ApplyUpdateResult`의
재시도 경로가 같은 per-window 타이머 슬롯을 `kOverlaySettleDelayMs`(150ms)로
다시 예약해 우리의 300ms 대기를 단축시켜 버리는 경합이 있었기 때문이다. 대신
`RevealMonitorMovePlacementIfPending`가 창을 보이게 하면 그때 발생하는
`EVENT_OBJECT_SHOW`가 자연스럽게 `DispatchUpdate`를 트리거하도록 맡긴다.

최대화 상태였던 창은 `ShowWindow(SW_MAXIMIZE)`로 대상 모니터 작업 영역에 맞춰
다시 계산되므로 이 숨김/재적용 대상에서 제외했다 (기존 방식 그대로 유지 —
실기기에서 별도 문제가 보고되지 않았고, 최대화 창의 중간 상태를 안전하게
숨기는 방법은 검증 없이 바꾸기엔 위험 부담이 커 보류했다).

### 3-2. 측정 스레드: `CreateThread` → OS 스레드 풀(`QueueUserWorkItem`)

`MeasureThreadProc`은 디스패치마다(코얼레싱으로 폭주는 막혀 있지만, 창이
여러 개거나 이벤트가 잦으면 여전히 빈번함) `CreateThread`로 새 OS 스레드를
만들고 바로 버렸다. 스레드 생성/종료는 몇 개의 DWM/GDI 호출로 이뤄지는 실제
작업량에 비해 커널 오버헤드(스택 예약, 커널 객체 생성/해제 등)가 큰 편이라,
CPU 점유율 관점에서 불필요한 비용이었다.

`CreateThread`/`CloseHandle`를 `QueueUserWorkItem(MeasureThreadProc, param,
WT_EXECUTELONGFUNCTION)`로 교체했다. 프로세스 공용 스레드 풀이 워커 스레드를
재사용하므로, 디스패치마다 스레드를 새로 만들고 없애는 비용이 사라지고
동작(각 창 독립적으로 비동기 처리, 결과는 `WM_OVERLAY_RESULT`로 메인 스레드에
전달)은 그대로 유지된다. `WT_EXECUTELONGFUNCTION`은 느리거나 멈춘 DWM
호출(기존 주석에서 언급된 "pathological case") 때문에 풀의 다른 작업이 밀리지
않도록 하는 힌트다.

### 검증

`MSBuild WindowPosButton.vcxproj /p:Configuration=Release /p:Platform=x64`로
정상 컴파일 확인. 실제 175%↔100% 모니터 간 이동 시 창/버튼 크기 육안 검증과,
장시간 사용 시 CPU 사용량 비교는 개발 환경 제약으로 수행하지 못했다.

## 남아 있는 항목 (참고)

- `g_locationEventLogs` 맵이 추적 대상이 아닌 창의 항목까지 계속 쌓이는 부분은
  메모리 이슈이며 CPU 이슈는 아니라 이번 작업에서는 보류했다.

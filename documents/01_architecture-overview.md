# WindowPosButton — 아키텍처 개요

> 이 문서는 처음 이 코드베이스를 보는 사람을 위한 것이다. `01_` → `02_` → `03_`
> 순서로 읽으면 "무엇을, 왜 이렇게 만들었는가"(01) → "구체적으로 어떻게
> 구현했는가"(02) → "특정 버그를 어떻게 고쳤는가"(03) 순으로 이해가 쌓이도록
> 구성했다.
>
> 대상 파일: `WindowPosButton/WindowPosButton.cpp` (단일 파일, 약 2,160줄,
> `namespace { ... }` 익명 네임스페이스 안에 거의 모든 코드가 들어있고
> `wWinMain`만 그 밖에 있다). 별도의 UI 프레임워크 없이 순수 Win32 API로
> 작성되었다.

## 1. 이 프로그램은 한 줄로 무엇인가

Windows에서 **모든 일반 창의 타이틀바, 최소화 버튼 왼쪽에 4개의 작은 버튼을
띄워주는** 상주형 유틸리티다.

- **스냅 좌/우**: 화면 폭의 50%(Shift+좌클릭 30%, 우클릭 70%)로 왼쪽/오른쪽에 배치
- **가운데 80%**: 작업 영역 폭의 80%(16:9 비율)로 중앙 배치, 우클릭 시 전체 높이
- **다음 모니터로 이동**: 디스플레이 배치 순서대로 다음 모니터로 이동(마지막 다음은
  처음으로 순환), 우클릭 시 모든 모니터에 걸쳐 확장
- (부가 기능) 창의 좌/우 리사이즈 테두리를 더블클릭하면 그 창을 가로로만 화면
  폭에 맞게 확장

시스템 트레이 아이콘으로 상주하며, "Windows 시작 시 자동 실행" 옵션을 제공한다.

## 2. 핵심 설계 제약과 그로부터 나온 선택들

이 섹션이 이 문서에서 가장 중요하다 — 아래 제약들이 이후 모든 아키텍처 결정을
설명한다.

### 2-1. "남의 창"을 조작해야 한다

이 앱이 다루는 대상 창은 전부 **다른 프로세스가 소유한 창**이다. 여기서 두 가지
근본적인 제약이 나온다.

1. **다른 프로세스의 창에 직접 그림을 그릴 수 없다.** 그래서 버튼은 대상 창의
   타이틀바 위에 "그려 넣는" 것이 아니라, 별도의 **독립적인 top-level 오버레이
   창**(레이어드, `WS_POPUP`)을 만들어 캡션 버튼(최소화/최대화/닫기) 바로 왼쪽에
   정확히 겹치도록 위치/크기를 맞춘다. 대상 창 하나당 4개의 오버레이 창(좌/우/
   가운데80/다음모니터)이 따로 존재한다 (`TrackedWindow`, `EnsureTracked`,
   `WindowPosButton.cpp:745`).
2. **Windows UIPI(User Interface Privilege Isolation)가 낮은 권한 프로세스의
   높은 권한(관리자 권한) 창 조작을 차단한다.** 사용자가 관리자 권한으로 실행한
   임의의 창(예: 일부 개발 도구, 설치 프로그램)에도 버튼이 동작하게 하려면 이
   앱 자체가 **항상 관리자 권한으로 실행**되어야 한다. 그래서:
   - 로그온 시 자동 시작을 레지스트리 `Run` 키가 아니라 **Windows 작업
     스케줄러(Task Scheduler)**로 등록한다 (`RegisterStartupTask`,
     `WindowPosButton.cpp:1670`, `TASK_LOGON_INTERACTIVE_TOKEN` +
     `TASK_RUNLEVEL_HIGHEST`). `Run` 키는 UAC 프롬프트 없이 관리자 권한으로
     자동 실행시킬 수 없기 때문이다.
   - About 대화상자에도 이 사실이 명시되어 있다 ("This application runs with
     administrator privileges so the buttons are available on elevated
     windows.", `ShowAboutWindow`, `:1851`).

### 2-2. 다른 프로세스에 침투하지 않는다

DLL 인젝션이나 후킹으로 대상 프로세스 내부에 들어가는 방식을 의도적으로
피했다.

- 창 이벤트 구독은 `SetWinEventHook`을 **`WINEVENT_OUTOFCONTEXT`**로 건다
  (`wWinMain`, `:2124`). `WINEVENT_INCONTEXT`였다면 이 프로세스의 DLL을 이벤트가
  발생하는 모든 프로세스에 주입해야 하지만, `OUTOFCONTEXT`는 이벤트를 이
  프로세스의 메시지 큐로 (비동기적으로) 전달해준다.
- 대상 창에 동기 `SendMessage`류 호출을 피한다. `PerformButtonAction` 주석:
  "Sending synchronous messages to target windows from this process can
  amplify UI stalls during COM/OLE-heavy workflows such as Unity launching
  Visual Studio."

### 2-3. 메인 스레드를 절대 막지 않는다

버튼 크기/위치를 계산하려면 `DwmGetWindowAttribute` 같은 크로스 프로세스 DWM
쿼리를 호출해야 하는데, 대상 프로세스가 응답 없는 상태(느리거나 멈춤)라면 이
호출이 오래 걸리거나 영영 안 끝날 수 있다. 그래서:

- 이런 쿼리는 항상 **OS 스레드 풀의 워커에서** 수행한다
  (`ComputeUpdatePlan`이 실제 작업, `SpawnMeasureThread`가
  `QueueUserWorkItem`으로 큐잉 — `:906`, `:927`). 결과는 `PostMessageW`로
  `WM_OVERLAY_RESULT`를 통해 메인 스레드로만 전달된다.
- 그 결과 **공유 상태를 보호할 락(lock)이 하나도 없다** — 워커 스레드는
  `g_windows`나 오버레이 HWND를 절대 건드리지 않고, 메인 스레드만 건드린다.
  (`ComputeUpdatePlan` 주석: "Never touches g_windows, TrackedWindow, or any
  OverlayButton, so it needs no lock — there is nothing shared to protect.")

### 2-4. 이벤트 기반, 폴링은 최소화

창의 이동/포커스/보임/숨김/소멸/Z-order 변경은 전부 OS 접근성 이벤트(WinEvent)를
구독해서 반응형으로 처리한다 (`WinEventProc`, `:1413`). 진짜 폴링(타이머로 주기적
확인)은 딱 두 곳뿐이다:

- **Z-order 안전망**(`kZOrderReassertTimerId`, 2초): `EVENT_OBJECT_REORDER`
  이벤트가 대부분의 경우를 이벤트 기반으로 이미 처리하므로, 이 타이머는 정말
  예외적인 경우를 위한 백업일 뿐이다.
- **수평 리사이즈 더블클릭 감지**(`kHorizontalResizeDoubleClickTimerId`,
  15ms): 전역 마우스 후크를 설치하거나 다른 프로세스 창에 `WM_NCHITTEST`를 보내는
  대신, 가벼운 `GetAsyncKeyState` 폴링으로 타협한 결과다.

## 3. 컴포넌트 지도

| 컴포넌트 | 핵심 함수/구조체 | 역할 |
|---|---|---|
| 부트스트랩 | `wWinMain` (`:2083`) | 단일 인스턴스 뮤텍스, DPI 인식 설정, 클래스 등록, 훅 설치, 메시지 루프 |
| 창 추적 | `TrackedWindow`, `g_windows`, `EnsureTracked`/`RemoveTracked`, `ShouldTrackWindow` | 어떤 창에 버튼을 붙일지 결정하고 상태를 보관 |
| 이벤트 파이프라인 | `WinEventProc`, `DispatchUpdate`, `ComputeUpdatePlan`, `ApplyUpdateResult` | 이벤트 → 비동기 측정 → 메인 스레드 적용 |
| 버튼 위치 계산 | `ComputeButtonRect` | 캡션 버튼 왼쪽 슬롯의 화면 좌표 계산 (DPI 포함) |
| 오버레이 렌더링 | `RenderAndPositionOverlay`, `SetPixelPremultiplied` | 레이어드 창에 버튼 아이콘을 픽셀 단위로 직접 그림 |
| Z-order 관리 | `RaiseOverlaysAboveTarget`, `ReassertForegroundZOrder`, `FindExternalFrontNeighbor` | 오버레이 버튼이 항상 대상 창 바로 앞, 다른 창에는 자연스럽게 가려지도록 유지 |
| 버튼 동작 | `PerformButtonAction`, `MoveWindowToNextMonitor`, `ExpandWindowAcrossAllMonitors` | 클릭 시 실제 창 이동/리사이즈 수행 |
| 수평 확장 | `GetHorizontalResizeEdge`, `PollHorizontalResizeDoubleClick`, `ExpandWindowHorizontally` | 리사이즈 테두리 더블클릭 감지 |
| 트레이 아이콘 | `CreateTrayIcon`/`RestoreTrayIcon`, `ShowTrayMenu`, `ScheduleTrayIconRecovery` | 알림 영역 아이콘, 우클릭 메뉴, Explorer 재시작/세션 전환 대응 |
| 자동 시작 | `IsStartupEnabled`, `RegisterStartupTask`/`DeleteStartupTask` | 작업 스케줄러 기반 로그온 자동 실행 (관리자 권한) |
| 진단 로깅 | `LogDiagnostic`/`LogWindowDiagnostic`, `InitializeDiagnosticLog` | `%LOCALAPPDATA%\WindowPosButton\diagnostic.log` |

## 4. 데이터 흐름 — 이벤트 하나가 버튼 갱신까지 가는 경로

```
OS WinEvent (예: 사용자가 창을 드래그)
   → WinEventProc (:1413)                 메인 스레드, 어떤 이벤트인지 분류
   → DispatchUpdate(hwnd) (:956)          메인 스레드, 이미 진행 중이면 코얼레싱만
   → SpawnMeasureThread (:927)            QueueUserWorkItem로 스레드 풀에 작업 제출
   → ComputeUpdatePlan (:683, 워커 스레드) ComputeButtonRect를 버튼마다 호출
   → PostMessageW(WM_OVERLAY_RESULT)      워커 → 메인 스레드로 결과만 전달
   → ApplyUpdateResult (:855, 메인 스레드) RenderAndPositionOverlay + SetWindowPos
```

버튼 클릭 자체는 이 파이프라인 밖에서 즉시 처리된다: 오버레이 창(`OverlayWndProc`,
`:1485`)이 `WM_LBUTTONUP`/`WM_RBUTTONUP`을 받으면 곧바로
`PerformButtonAction`(`:1353`)을 호출해 대상 창을 옮기고, 그 결과로 발생하는
`EVENT_OBJECT_LOCATIONCHANGE` 등이 위 파이프라인을 통해 오버레이 버튼 위치를
다시 계산한다 — 즉 "버튼을 눌러 창을 옮기는 것"과 "버튼 자신의 위치를 새로
계산하는 것"은 같은 이벤트 파이프라인을 공유한다.

## 5. 스레딩 모델 요약

- **메인 스레드**: 메시지 루프, 모든 오버레이 HWND, `g_windows`, 모든 타이머를
  소유. 오직 이 스레드만 오버레이 창에 `SetWindowPos`/`UpdateLayeredWindow`를
  호출한다 (`ApplyUpdateResult` 주석 참고).
- **OS 스레드 풀 워커**(`QueueUserWorkItem`): `ComputeUpdatePlan` 실행 — 대상
  창/모니터/DWM을 "읽기"만 하고, 결과를 값으로 복사해 메시지로 돌려보낸다.
- **동기화 프리미티브 없음**: 위 역할 분리 덕분에 뮤텍스/크리티컬 섹션이 전혀
  필요 없다. 유일한 교차 스레드 통신은 `PostMessageW(WM_OVERLAY_RESULT, ...)`
  하나뿐이다.

## 6. 어떤 창을 추적하는가

`ShouldTrackWindow` (`:357`)의 조건을 모두 만족해야 한다:

- 유효한 top-level 창이고 (`GetAncestor(hwnd, GA_ROOT) == hwnd`), 현재 보이는
  상태
- `WS_CAPTION` + `WS_MINIMIZEBOX` 스타일 보유 (일반적인 타이틀바+최소화 버튼이
  있는 창만)
- `WS_EX_TOOLWINDOW`가 아님 (툴윈도우/일부 팔레트 창 제외)
- Cloak되지 않음 (`DWMWA_CLOAKED`) — 다른 가상 데스크톱의 UWP 창 등 제외
- 창 사각형이 유효(0보다 큰 폭/높이)

이 프로세스 자신의 오버레이 창들은 `WS_POPUP`(캡션 없음)이라 이 조건에 자동으로
걸러진다.

## 7. 다음 읽을 문서

- **`02_technical-implementation.md`** — 위 컴포넌트들을 각각 더 구체적으로:
  이벤트 코얼레싱/타이머 재사용 메커니즘, 버튼 렌더링 방식, Z-order 유지
  방법, 트레이 아이콘 복구 로직, 작업 스케줄러 등록 세부사항 등.
- **`03_dpi-button-size-and-cpu-usage.md`** — 실제로 발견/수정한 세 가지
  구체적 문제의 기록: DPI가 다른 모니터 사이 버튼 크기 불일치, 진단 로깅으로
  인한 CPU/디스크 오버헤드, "다음 모니터로 이동" 시 비율 유지 및 화면 깜빡임
  제거.

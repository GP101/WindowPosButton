# WindowPosButton — 기술 구현 상세

> `01_architecture-overview.md`에서 소개한 컴포넌트들을 각각 구체적으로
> 다룬다. `03_dpi-button-size-and-cpu-usage.md`에서 다루는 세 가지 특정 버그
> 수정(DPI가 다른 모니터 사이 버튼 크기 불일치, 진단 로깅 CPU/디스크 오버헤드,
> "다음 모니터로 이동" 비율 유지·화면 깜빡임 제거)의 세부 내용은 여기서
> 반복하지 않고, 관련된 곳에서 03을 가리킨다.
>
> 코드 위치는 전부 `WindowPosButton/WindowPosButton.cpp` 기준이며, 향후 수정으로
> 줄 번호가 조금씩 밀릴 수 있다 — 정확한 위치는 함수명으로 다시 찾는 것을
> 권장한다.

## 1. 부트스트랩 (`wWinMain`, `:2083`)

시작 순서가 중요한 이유가 각 단계에 있다:

1. **단일 인스턴스 뮤텍스**(`CreateMutexW(kMutexName)`) — 이미 실행 중이면
   `ERROR_ALREADY_EXISTS`를 보고 안내 메시지 후 종료.
2. `InitializeDiagnosticLog()` — 이후 모든 단계에서 로그를 남길 수 있도록 가장
   먼저 초기화한다.
3. `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` —
   **반드시 어떤 창도 만들기 전에** 호출해야 한다.
4. `RegisterClasses()` — 오버레이용 클래스(`kOverlayClassName`)와 숨겨진 메인
   창용 클래스(`kMainClassName`) 등록.
5. `RegisterWindowMessageW(L"TaskbarCreated")` — Explorer가 재시작될 때 브로드
   캐스트하는 메시지를 등록해둔다 (§10 참고).
6. `g_hMain` 생성 — **일반 top-level 창으로 만들되 절대 보여주지 않는다.**
   메시지 전용 창(`HWND_MESSAGE` 부모)으로 만들지 않은 이유는 코드 주석에
   명시되어 있다: 메시지 전용 창은 `WM_DISPLAYCHANGE` 같은 브로드캐스트
   메시지를 받지 못하는데, 이 앱은 멀티모니터 핫플러그 대응에 그 메시지가
   필요하다.
7. `WTSRegisterSessionNotification` — 세션 잠금/해제 감지 (트레이 아이콘 복구용,
   §10).
8. `ScheduleTrayIconRecovery("startup")` — 로그온 트리거로 뜬 작업이 Explorer의
   알림 영역보다 먼저 시작될 수 있어서, 트레이 아이콘 등록도 재시도 경로를
   그대로 재사용한다.
9. 두 개의 반복 타이머 설치: Z-order 안전망(2초, §6) / 수평 리사이즈 더블클릭
   폴링(15ms, §9).
10. 5개의 `SetWinEventHook` 설치 — 전부 `hProcess=0, idThread=0`(시스템 전체),
    `WINEVENT_OUTOFCONTEXT` (`01_`의 2-2절 참고).
11. `EnumWindows`로 이미 떠 있는 모든 창을 한 번 훑어 `EnsureTracked` +
    `DispatchUpdate`.
12. 메시지 루프.
13. 종료 시: 5개 훅 해제 → 뮤텍스 해제/닫기 → `ShutdownDiagnosticLog()`(로그
    파일 닫기).

## 2. 창 추적 라이프사이클

### `TrackedWindow` (`:103`)

대상 창 하나당 하나씩, `g_windows`(`std::unordered_map<HWND,
std::unique_ptr<TrackedWindow>>`)에 보관된다.

- `OverlayButton left, right, nextMonitor, center80` — 각 버튼의 HWND와 상태
  (`hover`, `pressed`).
- `dispatchInFlight`, `dispatchAgainRequested` — 이벤트 코얼레싱용 (§3).
- `pendingMonitorMoveReassert`, `pendingMonitorMoveRect` — "다음 모니터로 이동"
  기능이 쓰는 필드. 세부 내용은 `03_`의 "다음 모니터로 이동" 절 참고.

### `EnsureTracked` (`:745`)

이미 추적 중이면 그대로 반환. 아니면 `TrackedWindow`를 만들고 4개의 오버레이
HWND를 생성한다:

```cpp
CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kOverlayClassName, L"", WS_POPUP, 0, 0, 1, 1, ...)
```

- `WS_POPUP` + 캡션 없음 → `ShouldTrackWindow`가 스스로를 다시 추적하지 않게
  자동으로 걸러진다.
- `WS_EX_NOACTIVATE` → 버튼을 클릭해도 오버레이 창 자신이 foreground가 되지
  않는다 (클릭 처리 후 `PerformButtonAction`이 대상 창에 `SetForegroundWindow`
  를 명시적으로 호출).
- **소유 창을 지정하지 않는다**(owner = `nullptr`). 코드 주석: "Cross-process
  owned popups can couple this helper's UI thread to COM/OLE-heavy apps such
  as Explorer, Unity, and Visual Studio during activation and shutdown." —
  대상 창을 owner로 지정하면 그 프로세스의 활성화/종료 처리에 이 프로세스의
  UI 스레드가 발이 묶일 수 있다는 뜻.
- 생성 직후에는 1×1 크기로 숨겨진 채로 있다가, 이벤트 파이프라인(§3)이 처음
  돌 때 실제 크기/위치로 갱신되고 보여진다.

### `RemoveTracked` / `HideOverlaysForTarget`

- `RemoveTracked`(`:1085`): `EVENT_OBJECT_DESTROY`에서 호출. 4개 오버레이
  `DestroyWindow` + `g_windows.erase` + `g_locationEventLogs.erase`.
- `HideOverlaysForTarget`(`:772`): `EVENT_SYSTEM_MINIMIZESTART`/
  `EVENT_OBJECT_HIDE`에서 호출. 4개 버튼을 `SW_HIDE`만 하고 `g_windows`
  엔트리는 그대로 둔다 (다시 보이면 재사용).

## 3. 이벤트 파이프라인

### `WinEventProc` (`:1413`) — 이벤트 라우팅

| 이벤트 | 처리 |
|---|---|
| `EVENT_SYSTEM_FOREGROUND` | `ShouldTrackWindow`면 `EnsureTracked` + **즉시** `RaiseOverlaysAboveTarget`(비동기 파이프라인을 기다리지 않고 Z-order만 그 자리에서 고침, §6) + `DispatchUpdate` |
| `EVENT_OBJECT_SHOW` | `EnsureTracked` + `DispatchUpdate` (추적 대상뿐 아니라 아무 창에나 옴) |
| `EVENT_OBJECT_LOCATIONCHANGE` | `DispatchUpdate`만. 로그는 초당 1줄로 스로틀 (`LogLocationEventThrottled`) |
| `EVENT_SYSTEM_MINIMIZESTART` / `EVENT_OBJECT_HIDE` | `HideOverlaysForTarget` |
| `EVENT_SYSTEM_MINIMIZEEND` | `DispatchUpdate` |
| `EVENT_OBJECT_DESTROY` | `RemoveTracked` |
| `EVENT_OBJECT_REORDER` | `ReassertForegroundZOrder` (§6). `hwnd`가 보통 데스크톱 컨테이너라 특정 창을 가리키지 않으므로, 현재 foreground 창에 대해서만 처리 |

모든 케이스 진입 전에 `idObject != OBJID_WINDOW`인 이벤트는 필터링된다.

### `DispatchUpdate` (`:956`) — 코얼레싱과 공유 타이머 슬롯

```cpp
void DispatchUpdate(HWND target, bool rearmSettleTimer = true) {
    auto it = g_windows.find(target);
    if (it == g_windows.end()) return;
    TrackedWindow& window = *it->second;

    if (rearmSettleTimer) {
        SetTimer(g_hMain, reinterpret_cast<UINT_PTR>(target), kOverlaySettleDelayMs, nullptr);
    }
    if (window.dispatchInFlight) {
        window.dispatchAgainRequested = true;
        return;
    }
    window.dispatchInFlight = true;
    SpawnMeasureThread(target);
}
```

- **코얼레싱**: 한 창에 대해 측정 스레드가 이미 도는 중이면(`dispatchInFlight`)
  새로 스레드를 만들지 않고 "끝나면 한 번 더" 플래그만 세운다
  (`dispatchAgainRequested`). 결과가 `WM_OVERLAY_RESULT`로 돌아왔을 때
  (`MainWndProc`, `:1929`) 이 플래그를 보고 다시 스폰할지 결정한다. 드래그처럼
  `EVENT_OBJECT_LOCATIONCHANGE`가 폭주하는 동안, 창 하나당 스레드가 딱 하나만
  떠 있게 보장하는 장치다.
- **공유 per-window 타이머 슬롯**: 이 코드베이스는 창(HWND)마다 필요한 여러
  개의 "이 창에 대해 잠시 후 뭔가 한 번 더 하고 싶다"는 요구를, `SetTimer`의
  ID로 **그 창의 HWND 값 자체**를 재사용해 하나의 슬롯으로 합친다 — 어차피
  진짜 HWND 값이 임의의 작은 정수 상수와 겹칠 일이 없으므로 안전하다(전역
  타이머 ID인 `kZOrderReassertTimerId`(1), `kTrayRecoveryTimerId`(2),
  `kHorizontalResizeDoubleClickTimerId`(3)과도 구분됨). `SetTimer`를 같은 ID로
  다시 호출하면 카운트다운이 리셋되므로, 여러 소비자가 이 슬롯을 "재무장"해도
  실제로는 마지막 호출의 지연 시간이 적용되고 딱 한 번만 발화한다. 현재 이
  슬롯을 쓰는 소비자는 세 곳이다:
  1. `ApplyUpdateResult`의 재시도(`retryLater`) — 막 복원된 창의 실제 위치가
     아직 안정되지 않았을 때.
  2. `DispatchUpdate` 자신의 `kOverlaySettleDelayMs`(150ms) 재무장 — 위 코드의
     `rearmSettleTimer` 분기.
  3. `MoveWindowToNextMonitor`가 쓰는 `kMonitorMoveReassertDelayMs`(300ms) —
     자세한 내용은 `03_` 참고.

  타이머가 발화하면 `MainWndProc`의 `WM_TIMER` 기본 분기(`:1966`)가 공통
  처리한다: `KillTimer` → `RevealMonitorMovePlacementIfPending`(3번 소비자가
  걸어놓은 게 있으면 처리, 없으면 즉시 반환) → `DispatchUpdate(target,
  rearmSettleTimer=false)`(2번 소비자가 하려던 일 — `false`를 주는 이유는, 이
  호출 자체가 이미 "확인 패스"이므로 스스로를 다시 무장하면 무한히 재발화하기
  때문).

### `ComputeUpdatePlan` (`:683`) — 워커 스레드에서 실행

`SpawnMeasureThread`가 스레드 풀에 제출하는 실제 작업. `target` HWND 하나만
받고, `g_windows`/오버레이 HWND는 절대 건드리지 않는다(§ `01_`-5).

- **준비 상태 판정**: `IsWindow && ShouldTrackWindow && !IsIconic && !stale`.
  `stale`은 `WindowRectLooksLikeMinimizedPlaceholder`(`:651`)로 판정 — 방금
  복원된 창이 `IsIconic()`은 이미 `false`를 보고하는데 `GetWindowRect`/DWM
  캡션 좌표는 아직 최소화 시절의 오프스크린 placeholder(대략
  `(-32000, -32000)`)를 반환하는 경우가 있어서, 이걸 그대로 믿으면 버튼이
  화면 밖에 그려진다. 이런 경우 `retryLater = true`를 세워 나중에 다시
  시도하도록 요청한다 (§ 위 공유 타이머 슬롯 1번 소비자).
- 준비됐으면 4개 버튼(SnapRight, SnapLeft, Center80, MoveNextMonitor — 단
  `MoveNextMonitor`는 모니터가 2개 이상일 때만) 각각에 대해
  `ComputeButtonRect`(§4)를 호출해 `ButtonPlacement{visible, rect}`를 채운
  `UpdateResult`를 반환한다.

### `ApplyUpdateResult` (`:855`) — 메인 스레드에서 적용

- **재검증**: 측정 스레드가 스냅샷을 뜬 시점과 메인 스레드가 결과를 받는
  시점 사이에 창이 최소화/종료됐을 수 있어, `IsWindow`/`ShouldTrackWindow`/
  `IsIconic`을 다시 한번 확인한다 — 이 재확인은 메시지를 보내지 않는 로컬
  API 호출뿐이라 비싸지 않다.
- 준비 안 됐으면 4개 버튼 모두 숨기고, `retryLater`면 위 공유 타이머 슬롯을
  무장.
- 준비됐으면, 보이는 버튼마다 `RenderAndPositionOverlay`(§5) + `SetWindowPos`
  로 실제 화면에 반영하면서, `FindExternalFrontNeighbor`로 찾은 Z-order
  위치에 순서대로 끼워 넣는다(§6과 동일한 삽입 로직).

## 4. 버튼 위치 계산 — `ComputeButtonRect` (`:392`)

캡션 버튼(최소화/최대화/닫기) 바로 왼쪽 슬롯의 화면 좌표를 계산한다.
`slot`(0, 1, 2, ...)이 클수록 더 왼쪽으로 밀린다.

1. **주 경로**: `DwmGetWindowAttribute(DWMWA_CAPTION_BUTTON_BOUNDS)`로 실제
   캡션 버튼 영역을 얻는다.
2. **대체 경로**(DWM 쿼리 실패 시): 96 DPI 기준 "타이틀바 높이 30px, 버튼
   폭 45px×3개, 우측 여백 8px"이라는 경험적 상수를 모니터 DPI 비율로
   스케일링해 추정한다.
3. 작업 영역(`GetMonitorInfoW`)을 벗어나지 않도록 상/하단을 클램프.
4. `shrink=true`(가운데80 버튼)면 계산된 정사각형을 80%로 축소.

DPI 처리(모니터 DPI와 창 자신의 DPI를 조합하는 스케일 계산, 그리고 그 계산이
드래그 도중 왜 불안정해질 수 있는지)는 `03_`의 1절에서 상세히 다룬다.

## 5. 오버레이 렌더링 — `RenderAndPositionOverlay` (`:500`)

- 32bpp `CreateDIBSection`으로 픽셀 버퍼를 직접 만들고, `SetPixelPremultiplied`
  (`:489`)로 전-곱연산(premultiplied) 알파를 한 픽셀씩 써 넣은 뒤
  `UpdateLayeredWindow`(`ULW_ALPHA`)로 한 번에 반영한다 — GDI 도형 함수 대신
  픽셀을 직접 쓰는 이유는 안티에일리어싱 없는 정확한 테두리/반투명 배경을
  얻기 위해서로 보인다.
- **hover/press 배경**: 마우스가 올라가 있으면 알파 55, 눌려 있으면 알파 90의
  검은 워시를 깔아 네이티브 캡션 버튼과 비슷한 피드백을 준다.
- **글리프 4종류** (전부 같은 바운딩 박스 안에 그림):
  - 스냅 좌/우: 사각형 테두리 + 절반을 액센트 블루로 채움 (좌/우에 따라 다른
    절반).
  - 가운데80: 바깥 테두리 + 중앙에 작은 채워진 사각형.
  - 다음 모니터: 나란한 두 모니터 아이콘, 오른쪽(목적지)만 채움.
  - (Center80이 아닌 스냅류) 테두리는 채워진 반쪽 위에도 항상 그려서, 타이틀바
    색이 액센트 블루와 같아도 버튼이 묻히지 않게 한다.
- 오버레이 창 클래스의 커서는 `IDC_HAND`로 고정(`RegisterClasses`).
- `OverlayWndProc`(`:1485`)이 `WM_MOUSEMOVE`/`WM_MOUSELEAVE`/
  `WM_LBUTTONDOWN`/`UP`/`WM_RBUTTONDOWN`/`UP`마다 `hover`/`pressed` 상태를
  갱신하고 그때그때 다시 그린다. `WM_LBUTTONUP`/`WM_RBUTTONUP`에서 눌린 채
  버튼 안에서 뗐으면(`PtInRect`) `PerformButtonAction`을 호출 — Shift+좌클릭은
  `ButtonVariant::ShiftLeftClick`, 우클릭은 `ButtonVariant::RightClick`로
  구분해서 넘긴다.

## 6. Z-order 유지

버튼은 대상 창 바로 앞에서, 대상 창을 실제로 가리는 다른 창에는 자연스럽게
가려져야 한다 — 즉 "항상 최상단"이 아니라 "대상 창 기준 상대적으로 최상단".

- `FindExternalFrontNeighbor`(`:786`): 대상 창에서 Z-order상 앞쪽으로
  올라가며, 우리 오버레이 클래스가 아닌 첫 창을 찾는다 — "실제로 지금 대상을
  가리고 있는 것이 뭔지" 알아내는 방법.
- `RaiseOverlaysAboveTarget`(`:808`): 4개 버튼을 그 이웃 바로 뒤(=대상 바로
  앞)에 순서대로 재삽입한다. 이미 올바른 순서면 아무것도 안 하고 조기 반환하는
  가드가 있는데, 이건 최적화일 뿐 아니라 정확성을 위한 것이기도 하다 — 이
  함수 자신이 호출하는 `SetWindowPos`도 `EVENT_OBJECT_REORDER`를 발생시켜
  다시 이 함수로 돌아오므로, 가드가 없으면 무한히 자기 자신을 재호출하게
  된다.
- `ReassertForegroundZOrder`(`:836`): 현재 foreground 창을 찾아 그 창에 대해서만
  `RaiseOverlaysAboveTarget`을 호출한다. `EVENT_OBJECT_REORDER`(데스크톱
  전체 Z-order 변경 시 발생)와 2초 안전망 타이머(`kZOrderReassertTimerId`)
  양쪽에서 호출된다 — 후자는 정말 예외적으로 REORDER 이벤트가 안 오는 경우를
  위한 백업일 뿐, 평소에는 이벤트 기반 경로가 다 처리한다.

## 7. 버튼 동작 — `PerformButtonAction` (`:1353`)

모든 액션 공통으로 시작 시 `SetForegroundWindow(hwnd)`를 호출한다(오버레이
버튼 자신은 `WS_EX_NOACTIVATE`라 이게 없으면 대상 창이 활성화되지 않음).

- **Center80**: `PrepareForManualResize`(최대화 상태면 먼저 `SW_RESTORE`) →
  작업 영역 폭의 80%, 높이는 16:9 비율(우클릭이면 작업 영역 전체 높이), 가로는
  항상 중앙 정렬(우클릭이면 세로는 상단 정렬).
- **SnapLeft/SnapRight**: `widthRatio`는 기본 0.5, Shift+좌클릭 0.3, 우클릭
  0.7. 높이는 항상 작업 영역 전체. `SetWindowPosForVisibleFrame`으로 배치
  (§8).
- **MoveNextMonitor**: 우클릭이면 `ExpandWindowAcrossAllMonitors`, 그 외엔
  `MoveWindowToNextMonitor` — 후자의 구체적인 비율 계산/화면 깜빡임 방지
  로직은 `03_` 참고.
- **ExpandWindowAcrossAllMonitors**(`:1146`): 최대화 상태면 먼저 복원.
  `SM_XVIRTUALSCREEN`/`SM_YVIRTUALSCREEN`/`SM_CXVIRTUALSCREEN`/
  `SM_CYVIRTUALSCREEN`으로 모든 모니터를 합친 가상 데스크톱 전체 사각형을
  구해 그대로 배치한다 — Windows의 일반 최대화는 모니터 하나에만 적용되므로,
  전체 확장에는 이 방법을 쓴다.

## 8. `SetWindowPosForVisibleFrame` / `GetVisibleFrameRect` — 공통 배치 패턴

`GetWindowRect`가 보고하는 사각형은 많은 DWM 관리 창에서 **보이지 않는
리사이즈 테두리**를 포함한다. 그래서 "작업 영역 왼쪽 끝에 딱 붙게" 배치하려고
`GetWindowRect` 기준으로 좌표를 맞추면, 실제로 보이는 프레임은 그 테두리만큼
안쪽으로 들어가 보인다.

- `GetVisibleFrameRect`(`:1183`): `DWMWA_EXTENDED_FRAME_BOUNDS`로 "진짜
  보이는" 프레임을 구하고, 실패하면 인자로 받은 폴백 사각형을 그대로 쓴다.
- `SetWindowPosForVisibleFrame`(`:1117`): 현재 `GetWindowRect`와
  `GetVisibleFrameRect`의 차이(inset)를 구해서, 원하는 "보이는" 사각형에 그
  inset을 다시 더한 실제 `SetWindowPos` 좌표를 계산해 호출한다.
- `GetWindowRectForVisibleFrame`(`:1192`)은 반대 방향 — 원하는 보이는
  사각형에서 raw `GetWindowRect`용 사각형을 역산한다(예: `WINDOWPLACEMENT.
  rcNormalPosition`에 넣을 값 계산).

이 패턴은 SnapLeft/SnapRight, `ExpandWindowHorizontally`, `MoveWindowToNextMonitor`
등 거의 모든 배치 함수에서 재사용된다.

## 9. 수평 리사이즈 더블클릭 확장

배경(`01_`-2-4절): 전역 마우스 후크나 다른 프로세스 창으로의 `WM_NCHITTEST`
전송을 피하기 위해, 가벼운 폴링으로 구현했다.

- `GetHorizontalResizeEdge`(`:975`): 커서가 현재 foreground 창의 좌/우
  리사이즈 테두리(`SM_CXSIZEFRAME + SM_CXPADDEDBORDER` 두께) 위에 있는지
  판정. 위/아래 코너 영역은 대각선 커서 영역이므로 제외.
- `HorizontalResizeDoubleClickState`(`:124`): 첫 클릭의 대상/모서리/위치/
  시각을 기록해두고, 두 번째 클릭이 `GetDoubleClickTime()`/`SM_CXDOUBLECLK`/
  `SM_CYDOUBLECLK` 기준을 만족하면 더블클릭으로 판정.
- `PollHorizontalResizeDoubleClick`(`:1031`, 15ms마다 호출): `GetAsyncKeyState
  (VK_LBUTTON)`로 좌클릭 버튼의 눌림/뗌 전이를 감지해 위 상태 머신을 굴리고,
  더블클릭이 확정되면(마우스를 뗀 시점) `ExpandWindowHorizontally`를 호출.
- `ExpandWindowHorizontally`(`:999`): 세로(위/아래)는 그대로 두고, 가로만
  작업 영역 전체 폭으로 확장 — Windows 기본 동작인 "상단 테두리 더블클릭 →
  세로로만 최대화"의 가로 버전.

## 10. 트레이 아이콘 수명주기

- `CreateTrayIcon`(`:2023`): `Shell_NotifyIconW(NIM_ADD)` 후
  `NIM_SETVERSION`으로 `NOTIFYICON_VERSION_4`를 요청 — 최신 알림 동작(예:
  `WM_CONTEXTMENU` 통지)을 쓰기 위함.
- `WM_TRAYICON` 처리(`MainWndProc`): `NOTIFYICON_VERSION_4`에서는 알림 코드가
  `lParam`의 하위 워드에 들어오므로(상위 워드는 커서 좌표), `LOWORD(lParam)`으로
  비교해야 좌표가 (0,0)이 아닌 우클릭도 놓치지 않는다.
- `ShowTrayMenu`(`:1828`): About / "Start automatically with Windows"(현재
  상태를 체크박스로 반영, §11) / Exit. 메뉴를 띄우기 전 `SetForegroundWindow`,
  띄운 뒤 `PostMessage(WM_NULL)` — 후자는 메뉴가 포커스를 잃어도 제대로
  닫히도록 하는 표준 관용구.
- **복구 로직**(`ScheduleTrayIconRecovery`, `:1870`): 아래 세 상황 모두
  같은 경로를 탄다 — `RestoreTrayIcon()` 즉시 1회 호출 +
  `kTrayRecoveryRetryCount`(4회)까지 `kTrayRecoveryRetryIntervalMs`(500ms)
  간격으로 재시도.
  1. Explorer가 재시작되며 브로드캐스트하는 `TaskbarCreated` 메시지 수신 시
     (Explorer 재시작은 이 프로세스가 등록해둔 아이콘을 전부 지워버린다).
  2. `WM_WTSSESSION_CHANGE`의 `WTS_SESSION_UNLOCK` (화면 잠금 해제).
  3. `WM_POWERBROADCAST`의 절전 복귀(`PBT_APMRESUMEAUTOMATIC`/
     `PBT_APMRESUMESUSPEND`).
  4. 프로세스 시작 시 자체적으로도 한 번(`wWinMain`) — 로그온 트리거로 뜬
     작업이 Explorer의 알림 영역 초기화보다 먼저 실행될 수 있어서.
- `RestoreTrayIcon`(`:2049`): 먼저 `NIM_MODIFY`를 시도(등록이 살아있으면
  그걸로 충분), 실패하면 `NIM_ADD`부터 다시.

## 11. 자동 시작 — Windows 작업 스케줄러 (`RegisterStartupTask` 등)

`01_`-2-1절에서 설명한 대로, 관리자 권한 자동 실행을 위해 레지스트리 `Run` 키
대신 작업 스케줄러를 쓴다.

- `ConnectTaskScheduler`(`:1580`): `ITaskService`/`ITaskFolder` COM 객체를
  얻어 루트(`\`) 폴더에 연결.
- `IsStartupEnabled`(`:1608`): 태스크가 존재하고, `Enabled`이고, 그 안의
  `IExecAction` 경로가 **현재 실행 파일 경로와 일치**하는지까지 확인한다 —
  실행 파일을 다른 경로로 옮겼다면 "꺼짐"으로 보이는 게 의도된 동작이다.
- `RegisterStartupTask`(`:1670`):
  - `IPrincipal`: `TASK_LOGON_INTERACTIVE_TOKEN` + `TASK_RUNLEVEL_HIGHEST` —
    관리자 권한으로 상호작용형 로그온 시 실행.
  - `ITaskSettings`: `Enabled`, `StartWhenAvailable`, 배터리 여부와 무관하게
    시작/중단하지 않음.
  - 트리거: `TASK_TRIGGER_LOGON`.
  - 액션: `TASK_ACTION_EXEC`로 현재 실행 파일 경로(`GetCurrentExecutablePath`).
  - `RegisterTaskDefinition(..., TASK_CREATE_OR_UPDATE, ...)`로 등록/갱신.
- `DeleteStartupTask`(`:1793`): 태스크 삭제. 파일이 없다는 에러
  (`ERROR_FILE_NOT_FOUND`)는 이미 없는 것으로 간주해 성공 처리.
- `DeleteLegacyStartupValue`(`:1572`): 예전 버전이 레지스트리 `Run` 키
  (`kLegacyRunKeyPath`)를 쓰던 흔적이 있으면 정리 — 등록/삭제 양쪽에서 호출.
- 트레이 메뉴에서 토글 시 실패하면 `MessageBoxW`로 안내(`SetStartupEnabled`,
  `:1818`).

## 12. 진단 로깅

- 한 줄 형식: `YYYY-MM-DD HH:MM:SS.mmm tid=<스레드ID> event=<이벤트명>
  hwnd=0x<주소> pid=<프로세스ID> <추가정보>` (`LogDiagnostic`/
  `LogWindowDiagnostic`, `:234`/`:255`).
- 파일 위치: `%LOCALAPPDATA%\WindowPosButton\diagnostic.log`. 시작할 때마다
  기존 로그를 `diagnostic-prev.log`로 옮기고(1세대만 보관) 새로 시작한다
  (`InitializeDiagnosticLog`, `:274`).
- `LogLocationEventThrottled`(`:317`)/`LogReorderEventThrottled`(`:337`)은
  각각 `EVENT_OBJECT_LOCATIONCHANGE`/`EVENT_OBJECT_REORDER`처럼 폭주하기
  쉬운 이벤트를 초당 1줄(발생 횟수 요약)로 눌러서 기록한다.
- 로깅은 릴리스 빌드에서도 항상 켜져 있다(조건부 컴파일 없음). 이 특성이 실제
  성능 문제로 이어졌던 사례와 그 수정(로그 파일을 매번 열고 닫지 않고 상시
  유지, REORDER 스로틀링 추가)은 `03_`의 2절 참고.

## 13. 리소스와 빌드

- `WindowPosButton.rc`: 앱 아이콘 리소스만 포함 — 별도의 `VERSIONINFO`
  리소스는 없고, 버전 문자열은 코드 상수 `kAppVersion` 하나로 관리된다(`:79`).
  DPI 인식도 매니페스트가 아니라 코드(`SetProcessDpiAwarenessContext`)로
  설정한다.
- 링크 라이브러리: `dwmapi`, `ole32`, `oleaut32`, `shell32`, `taskschd`,
  `shcore` (파일 상단 `#pragma comment(lib, ...)`).

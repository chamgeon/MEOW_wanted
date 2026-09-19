# Vampire-Core Wasm (Team MEOW)

> **Live backend**: https://meow-ai.p-e.kr/
> 상태 확인: [`/health`](https://meow-ai.p-e.kr/health) · [`/api/agent-meta`](https://meow-ai.p-e.kr/api/agent-meta)


C++ 시뮬레이션 코어를 WebAssembly로 컴파일해 브라우저에서 **엔티티 1만 개를 60 FPS로** 돌리는 성능 테크 데모입니다. 화면 위에서 두 가지 최적화 축을 실시간으로 켜고 끄면서 프레임 타임이 어떻게 변하는지 직접 볼 수 있습니다.

| 토글 | A | B | 무엇을 보여주나 |
|---|---|---|---|
| **Collision** | Brute Force `O(N²)` | Dynamic QuadTree `O(N log N)` | 알고리즘 복잡도 |
| **Memory** | AoS (Array of Structures) | SoA (Structure of Arrays) | 캐시 지역성 / DOD |

---

## 준비물

| | 용도 | 필수 여부 |
|---|---|---|
| **Node.js 18+** | 웹 클라이언트 | 필수 |
| **Emscripten SDK** | C++ → Wasm 컴파일 | 필수 |
| **Git** | emsdk 설치에 필요 | 필수 |
| C++ 컴파일러 (MSVC / gcc / clang) + CMake | 네이티브 벤치마크 `core_bench` | 선택 |

> Wasm 산출물(`web/public/core_engine.js`, `.wasm`)은 저장소에 커밋돼 있지 않습니다. 각자 한 번 빌드해야 합니다.

---

## 빠른 시작 — Windows (PowerShell)

### 1. PowerShell 스크립트 실행 허용 (최초 1회)

emsdk와 이 저장소의 빌드 스크립트가 전부 `.ps1`입니다. 기본 정책이 이를 막습니다.

```powershell
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned -Force
```

`RemoteSigned`는 로컬에서 만들었거나 `git clone`으로 받은 스크립트만 허용하고, 브라우저로 내려받은 `.ps1`은 계속 서명을 요구합니다. 관리자 권한이 필요 없고 영구 적용됩니다.

### 2. Emscripten SDK 설치 (최초 1회)

```powershell
cd D:\GitHub          # 원하는 위치 아무 데나
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
.\emsdk install latest
.\emsdk activate latest
```

수 GB를 내려받으므로 몇 분 걸립니다. 마지막에 `Done installing SDK ...`가 나와야 성공입니다.

### 3. SDK 활성화 (**터미널을 새로 열 때마다 매번**)

```powershell
cd D:\GitHub\emsdk
.\emsdk_env.ps1
Get-Command em++     # 경로가 출력되면 OK
```

`emsdk_env.ps1`은 **지금 이 셸의 PATH만** 수정합니다. 창을 닫으면 사라지므로, 빌드할 때마다 이 단계를 먼저 거쳐야 합니다.

### 4. Wasm 빌드

```powershell
cd D:\GitHub\wanted_demo\vampire-core-wasm
.\build-wasm.ps1
```

`web\public\core_engine.js`와 `core_engine.wasm`이 생기고 파일 크기가 출력됩니다.

### 5. 웹 클라이언트 실행

```powershell
cd web
npm install          # 최초 1회
npm run dev
```

브라우저에서 <http://localhost:5173> 을 엽니다.

---

## macOS / Linux

같은 순서지만 1단계(ExecutionPolicy)가 필요 없고, `Makefile`을 쓸 수 있습니다.

```bash
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh          # 새 셸마다 매번

cd /path/to/vampire-core-wasm
make wasm                      # web/public/ 으로 산출물 복사
make dev                       # Vite 개발 서버
```

`make wasm`은 `emcmake cmake`를 씁니다. Windows에서 이게 실패하는 이유는 아래 트러블슈팅을 보세요.

---

## 조작법과 관전 포인트

- **이동**: `WASD` 또는 방향키
- **Enemies 슬라이더**: 500 ~ 10,000
- **Collision / Memory 버튼**: 클릭할 때마다 모드 전환

HUD는 서로 혼동하기 쉬운 두 숫자를 분리해서 보여줍니다.

- **Sim ms** — `EngineCore::tick`만, C++ 내부에서 30프레임 롤링 윈도로 측정. 최적화 작업이 실제로 움직이는 숫자입니다.
- **Frame ms** — tick + 캔버스 블릿까지 포함한 JS 프레임 전체. C++ 쪽에서는 볼 수 없는 비용이 들어 있습니다.

토글을 누르면 두 값 모두 리셋됩니다. 누적 평균이면 전환이 몇 초에 걸쳐 뭉개져서, 데모에서 가장 중요한 상호작용이 아무 반응 없는 것처럼 보이기 때문입니다.

**추천 시연 순서**

1. 슬라이더를 10,000까지 올리고 Collision은 QuadTree로 둡니다. 60 FPS를 유지합니다.
2. Collision을 **BruteForce**로 전환. Sim ms가 한 자릿수 배로 뜁니다 — 프레임마다 5천만 번의 쌍 검사입니다. Frame ms는 같은 **절대량**만 오릅니다. 렌더 비용은 그대로이기 때문이고, 알고리즘 개선분이 전부 Sim 열에 있다는 뜻입니다.
3. BruteForce 상태에서 Memory를 **SoA ↔ AoS**로 전환. 여기서 쌍 루프가 메모리 바운드라 캐시 효과가 드러납니다. 다만 격차 크기는 **본인 CPU의 L2 크기에 달려 있습니다**: 엔티티 1만 개면 AoS가 1.28 MB이므로, L2가 그보다 크면 여전히 캐시에 들어가서 두 모드가 비깁니다. 그럴 땐 슬라이더를 더 올려 AoS를 캐시 밖으로 밀어내면 격차가 벌어집니다.
4. QuadTree 상태에서 Memory를 전환해 보면 거의 차이가 없습니다. 쌍 개수가 이미 줄어들어 루프가 대역폭 바운드가 아니라 레이턴시 바운드가 됐기 때문입니다. 원리상 곱해질 것 같은 두 최적화가 실제로는 서로를 가릴 수 있다는 것을 그대로 보여줍니다.

---

## 트러블슈팅

**`.\emsdk_env.ps1 : 이 시스템에서 스크립트를 실행할 수 없으므로 ...`**
ExecutionPolicy입니다. 위 1단계를 실행하세요. 정책을 바꾸고 싶지 않다면 그 세션에서만: `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force`.

**`emcc' 용어가 ... 인식되지 않습니다` / `ERROR: em++ not found on PATH`**
같은 셸에서 `emsdk_env.ps1`을 실행하지 않았습니다(3단계). 그래도 안 되면 설치가 안 끝난 것이니 `Test-Path D:\GitHub\emsdk\upstream\emscripten\emcc.py`로 확인하고 2단계를 다시 돌리세요.

**`wasm-ld: error: undefined symbol: operator new(unsigned long)`**
`emcc`로 링크하면 납니다. `emcc`는 `.cpp`를 C++로 컴파일하지만 **C 드라이버로 링크**해서 libc++가 링크 라인에서 빠지고, `new`/`delete`/예외 심볼이 전부 미해결로 남습니다. 컴파일은 통과하기 때문에 소스 문제처럼 보이지만 순전히 드라이버 선택 문제입니다. `em++`를 쓰세요 — `build-wasm.ps1`은 이미 그렇게 돼 있습니다.

**Windows에서 `emcmake cmake`가 실패**
CMake의 기본 제너레이터가 Visual Studio인데, VS 제너레이터는 clang/emcc를 구동하지 못합니다. Ninja나 MinGW make를 PATH에 올리면 우회되지만, 이 프로젝트는 번역 단위가 4개뿐이라 `build-wasm.ps1`이 `em++`를 직접 호출합니다. 제너레이터가 잘못될 여지 자체가 없습니다.

**화면에 `Wasm not loaded — build core first` 배너**
`web/public/core_engine.js`가 없습니다. 4단계를 실행하세요. 앱은 이 상태에서 크래시하지 않고 배너만 띄웁니다.

**5173 포트가 이미 사용 중**
Vite가 자동으로 5174 등으로 올라갑니다. 터미널에 찍힌 실제 주소를 보세요.

**엔티티가 스폰 위치에 얼어붙어 보임**
정상 빌드에서는 발생하지 않습니다. 구형 `core_engine.wasm`이 남아 있을 수 있으니 `web/public/core_engine.*`를 지우고 4단계를 다시 하세요.

---

## 네이티브 벤치마크 (선택)

브라우저 없이 C++ 코어만 측정하고 검증합니다. Emscripten이 필요 없습니다.

```powershell
cd core
cmake -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --config Release
.\build-native\Release\core_bench.exe
```

macOS/Linux는 저장소 루트에서 `make bench`.

실행하면 이 머신의 실제 L1/L2/L3 크기, AoS/SoA 엔티티 바이트 수, 캐시 스트라이드 스윕, 그리고 **7개의 자동 검증**을 출력합니다. 이 중 `AoS/SoA equivalence`가 가장 중요합니다 — 같은 시드에서 두 레이아웃을 각각 틱한 뒤 위치가 `1e-3` 이내로 일치하는지 확인합니다.

**성능 수치를 인용하기 전에 반드시 이 검증이 전부 PASS인지 확인하세요.** 한쪽 경로가 조용히 일을 덜 해서 나온 speedup은 숫자가 없느니만 못합니다. 실제로 이 프로젝트에서 QuadTree가 밀집 구간의 96%를 누락해서 빨라 보였던 적이 있고, 전역 `rand()` 공유 때문에 두 레이아웃이 서로 다른 세계를 시뮬레이션한 적도 있습니다. 둘 다 이 검증이 잡아냈습니다.

한쪽 레이아웃에만 필드나 시스템을 추가하면 검증이 깨집니다. 반드시 양쪽에 반영하세요.

---

## 디렉터리 구조

```
vampire-core-wasm/
├── core/                     # 순수 C++ 시뮬레이션 엔진
│   ├── include/              # Config, Vector2D, EnemyAoS, EnemySoA, QuadTree, EngineCore
│   └── src/
│       ├── EngineCore.cpp    # 게임 루프, 반발력, 투사체
│       ├── QuadTree.cpp      # 동적 공간 분할
│       ├── bindings.cpp      # Emscripten embind + 선형 메모리 포인터 노출
│       └── main.cpp          # 네이티브 벤치마크 + 검증 하네스
├── web/                      # 프론트엔드 뷰어 & 프로파일러 HUD
│   ├── public/               # ← Wasm 산출물이 여기로 (빌드 전에는 비어 있음)
│   └── src/
│       ├── App.tsx           # 단일 rAF 루프, 프레임 회계
│       └── components/       # GameCanvas, MetricGraph, CodePanel
├── server/                   # FastAPI AI 코드 프로파일러 백엔드
├── build-wasm.ps1            # Windows 원샷 Wasm 빌드
└── Makefile                  # macOS/Linux: make wasm / bench / dev / server
```

### 렌더링이 zero-copy인 이유

C++는 JS로 데이터를 직렬화해 넘기지 않습니다. Wasm 힙 안에 있는 연속 float 배열의 **정수 주소**만 건네고, JS가 `HEAPF32`의 `ptr >>> 2` 위치에서 직접 읽습니다. 엔티티 1만 개 기준으로 프레임당 포인터 읽기 2번과 마샬링되는 double 2만 개의 차이입니다.

주소는 힙이 성장하는 순간 무효화되므로 **매 프레임 다시 읽어야 합니다**. 특히 `setEnemyCount()`는 모든 배열을 재할당합니다.

---

## AI 프로파일러 백엔드 (선택)

런타임 성능 통계와 C++ 소스 스니펫을 받아 병목 진단과 리팩터링 diff를 돌려주는 FastAPI 서버입니다.

```bash
cd server
pip install -r requirements.txt
cp .env.example .env          #   OPENAI_API_KEY 채우기
uvicorn main:app --reload --port 8000
```

Vite 개발 서버가 `/api`를 `localhost:8000`으로 프록시합니다. `.env`는 gitignore돼 있습니다 — **OpenAI API 키를 절대 커밋하지 마세요.**


> 로컬 세팅이 번거로우면 배포된 인스턴스(`https://meow-ai.p-e.kr/`)를 바로 써도 됩니다. 상태 확인: [`/health`](https://meow-ai.p-e.kr/health) · [`/api/agent-meta`](https://meow-ai.p-e.kr/api/agent-meta)


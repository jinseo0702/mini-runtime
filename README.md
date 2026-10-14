# Mini Accelerator Runtime

**C++20 비동기 작업 실행 Runtime의 핵심 동작을 구현하고 검증하는 프로젝트입니다.**
작업을 제출하면 Scheduler가 Backend에 전달하고, 완료 callback을 Completion worker가 처리해
Event 결과를 게시합니다. 애플리케이션은 Event로 진행 상태를 조회하거나 완료를 기다릴 수 있습니다.

현재는 실제 GPU 연산 대신 **MockBackend의 `test.noop` v1**을 실행합니다.
명령 소유권, 동시 호출, 중복 callback 처리, 메모리 회수, 시작 실패 정리와 안전한 종료를
작은 실행 모델에서 확인하는 단계입니다. Runtime은 `include/mini_runtime/`의 헤더 라이브러리이며,
별도 라이브러리 파일을 링크할 필요가 없습니다.

## 바로 빌드하고 실행하기

필요한 도구는 Linux 환경의 C++20 컴파일러, GNU Make, GNU coreutils의 `timeout`입니다.
기본 컴파일러는 `g++`이고, 표준 라이브러리와 스레드 지원 외에 추가 라이브러리는 필요하지 않습니다.
검증 환경은 GCC 15.2.0, Clang 21, GNU Make 4.4.1입니다.

```sh
git clone --branch docs/build-and-readme git@github.com:jinseo0702/mini-runtime.git
cd mini-runtime
make -j2
make run
make test
```

`make`는 예제 실행 파일과 테스트 프로그램 16개를 빌드합니다.
`make run`은 실제 Runtime과 MockBackend를 시작하고 작업 하나를 제출한 뒤 완료를 기다리고 종료합니다.

```text
Runtime started: runtime_id=1
Submitted test.noop v1: event_id=1
Event completed: COMPLETED
Event released; runtime stopped: STOPPED
```

ID는 예제 프로세스에서 생성된 값입니다. 예제 전체 코드는 [examples/noop.cpp](examples/noop.cpp)에 있습니다.

| 명령 | 동작 |
| --- | --- |
| `make` | 예제와 모든 테스트 프로그램 빌드 |
| `make demo` | 예제만 빌드 |
| `make run` | 예제 빌드 후 실행 |
| `make tests` | 테스트 프로그램만 빌드 |
| `make test` | 테스트 빌드 후 순서대로 실행, 첫 실패에서 중단 |
| `make test-asan` | ASan·UBSan·LeakSanitizer가 활성화된 테스트 빌드 및 실행 |
| `make test-tsan` | ThreadSanitizer 테스트 빌드 및 실행 |
| `make clean` | `.build/`의 생성 파일 삭제 |
| `make help` | 명령 도움말 |

빌드 결과는 `.build/normal/`, `.build/asan/`, `.build/tsan/`으로 나뉩니다.
헤더 변경이나 컴파일러·빌드 옵션 변경 시 필요한 실행 파일을 다시 빌드합니다.

```sh
make -j2 CXX=clang++
make -j2 CXXFLAGS='-O2 -g'
make test TEST_TIMEOUT=180s
```

기본 테스트 제한 시간은 프로그램별 90초입니다. 컴파일 실패, assertion 실패,
sanitizer 오류, 제한 시간 초과는 모두 `make` 실패로 반환됩니다.
테스트의 assertion은 사용자 `CXXFLAGS`와 관계없이 활성화합니다.

## 애플리케이션에서 사용하기

아래 순서로 사용합니다. 각 `Result`의 성공 여부를 확인한 뒤 `value()` 또는 `error()`를 읽습니다.

```cpp
#include "mini_runtime/mock_backend.hpp"
#include "mini_runtime/runtime.hpp"
#include <memory>

int main() {
    using namespace mini_runtime;
    auto control = std::make_shared<testing::MockControl>();
    auto factory = std::make_shared<testing::MockBackendFactory>(control);
    Runtime runtime(factory);

    auto started = runtime.start();
    if (!started) return 1;

    CommandDescriptor command{{"test.noop", 1}, {}, {}, {}, {}};
    auto submitted = runtime.submit(command);
    if (!submitted) return 1;

    auto event = submitted.value();
    auto snapshot = runtime.wait(event);
    if (!snapshot || snapshot.value().status != EventStatus::COMPLETED) return 1;

    auto released = runtime.release_event(event);
    if (!released) return 1;
    return runtime.shutdown() ? 0 : 1;
}
```

오류 처리까지 포함한 실행 예제는 [examples/noop.cpp](examples/noop.cpp)를 참고하세요.
다른 프로그램에서는 `-std=c++20 -pthread -Iinclude` 옵션으로 헤더를 사용할 수 있습니다.

| API | 역할 |
| --- | --- |
| `start()` | Backend와 Scheduler·Completion worker 초기화 |
| `submit(command)` | 명령 내용을 복사해 소유하고 EventHandle 반환 |
| `query(event)` | 현재 상태를 즉시 조회 |
| `wait(event)` | Event가 최종 상태가 될 때까지 대기 |
| `release_event(event)` | 외부에서 접근하는 EventHandle 해제 |
| `shutdown()` | 새 제출 차단, 대기 작업 취소, 진행 중 작업 정리, worker 종료 |
| `state()`, `runtime_id()` | Runtime 상태 및 식별자 조회 |

Event 상태는 `PENDING`, `COMPLETED`, `FAILED`, `CANCELLED`입니다.
`wait()`의 `Result` 성공은 결과 조회의 성공이므로, 실제 작업 성공은
`snapshot.status == EventStatus::COMPLETED`로 판단합니다.

완료한 Event는 `release_event()` 전까지 조회할 수 있고, 정상 `shutdown()` 이후에도 결과가 남습니다.
해제한 Event나 다른 Runtime의 EventHandle에는 `INVALID_EVENT`를 반환합니다.
동시 호출 중에는 호출을 마칠 때까지 Runtime 객체를 살아 있게 유지해야 합니다.
Runtime은 복사·이동할 수 없으며, `STOPPED` 이후 다시 `start()`할 수 없습니다.

현재 허용되는 명령은 `OperationKey{"test.noop", 1}`이고,
`parameters`, `inputs`, `outputs`, `dependencies`는 모두 비어 있어야 합니다.
등록되지 않은 operation은 `UNKNOWN_OPERATION`, 지원하지 않는 명령 내용은 `INVALID_PARAMETER`로 거절합니다.

## 동작 구조

```text
애플리케이션
  submit() → 명령 복사 · Event 생성 · READY queue
                                  ↓
                            Scheduler worker
                                  ↓
                              MockBackend
                                  ↓ callback
                           Completion queue
                                  ↓
                           Completion worker
                                  ↓
                      EventSnapshot 게시 · waiter 깨우기
```

Runtime 내부 상태는 하나의 Coordinator mutex로 보호하고,
Backend 호출 및 thread join은 해당 mutex를 잡지 않은 상태에서 수행합니다.
작업마다 명령과 완료 처리에 필요한 객체를 소유하며, 완료가 게시된 후에도 callback과
Backend의 참조가 실제로 해제될 때까지 필요한 객체를 유지합니다.

정상 생명주기는 `CREATED → STARTING → RUNNING → SHUTTING_DOWN → DRAINING → STOPPED`입니다.
시작 중 실패하면 생성한 자원을 정리하고 `START_FAILED`로 전환합니다.
종료 시 READY 작업은 취소하고, Backend가 수락한 작업의 callback 반환과 참조 해제를 기다린 뒤
Completion 처리를 마치고 worker를 종료합니다. Runtime 내부 timeout이나 장애 복구는 현재 범위에 없습니다.

## 파일 구성

| 경로 | 내용 |
| --- | --- |
| `include/mini_runtime/runtime.hpp` | 애플리케이션용 Runtime API |
| `result.hpp`, `runtime_error.hpp`, `core_types.hpp` | 결과·오류·명령 자료형 |
| `backend.hpp`, `mock_backend.hpp`, `mock_control.hpp` | Backend 계약과 테스트용 실행·제어 |
| `lifecycle_policy.hpp`, `lifecycle_gate.hpp`, `startup.hpp` | 상태 전이와 시작·종료 준비 |
| `event_registry.hpp`, `work_store.hpp` | Event, 명령 소유권, 작업 저장소 |
| `dispatch.hpp`, `completion_processor.hpp` | Scheduler 전달과 완료 처리 |
| `include/mini_runtime/testing/runtime_harness.hpp` | 내부 상태 관찰 및 결정적인 테스트 제어 |
| `tests/unit/` | 단위·통합·수락·장기 반복 테스트 프로그램 16개 |
| `examples/noop.cpp` | 실제 실행 가능한 최소 사용 예제 |
| `Makefile` | 빌드·실행·테스트 자동화 |

표에서 파일명만 적은 헤더는 모두 `include/mini_runtime/` 아래에 있습니다.
GPU Backend, Buffer, Graph, Snapshot, IPC 기능은 현재 구현 범위에 포함되지 않습니다.

## 검증 범위와 현재 상태

2026-10-14 기준 T01~T09의 작업 16개가 독립 Supervisor 리뷰와 Controller 검사로 수락되었습니다.
제품 코드 기준 commit은 `fc927fb03d718e57280a490a228a4286890c1f8d`이며,
`sdd/accepted` 브랜치에 보존되어 있습니다. 마지막 Controller 독립 검사 45개는 모두 PASS이고,
일반 실행, ASan·UBSan·LeakSanitizer, TSan, Clang 정적 분석 결과를 포함합니다.
T02~T09의 최종 Human Milestone 검토는 별도로 남아 있습니다.

장기 반복 검사는 직렬 작업 10,000개와 호출자 4명의 동시 작업 1,000개를 수행하고,
작업별 메모리 회수 경계, 남은 자원 수, 종료 및 worker 정리를 확인합니다.
표시되는 실행 시간은 관측값이며 성능 보장이나 SLA는 아닙니다.

이 저장소의 `make test`는 저장소에 포함된 테스트 프로그램을 직접 실행합니다.
Controller의 공식 검사는 저장소 밖의 보호 fixture와 C allocation probe도 실행하고,
원본 stdout/stderr와 hash를 보관합니다. Makefile 결과가 공식 수락 절차를 대신하지 않습니다.

`make test-asan`과 `make test-tsan`은 현재 실행 환경에서 sanitizer를 직접 실행합니다.
ASan 모드는 leak 검사를 항상 활성화하고, 모든 sanitizer 오류를 실패로 반환합니다.
현재 운영 머신의 제한된 실행 환경에서는 native LeakSanitizer 실행이 실패한 기록이 있어,
공식 ASan·UBSan·LSan 검사는 기존 sandbox 안의 격리된 Linux guest에서 검증했습니다.
native 실행 환경이 LeakSanitizer를 지원하지 않으면 `make test-asan`도 실패하며,
해당 실패를 제품의 메모리 누수 확정이나 PASS로 해석하지 않습니다.

2026-10-15에 이 브랜치의 사용 명령을 확인한 결과입니다.

| 확인한 명령 | 실제 결과 |
| --- | --- |
| `make -j2` | 예제와 테스트 프로그램 16개 빌드 성공 |
| `make run` | 작업 완료, Event 해제, 정상 종료 성공 |
| `make test` | 테스트 프로그램 16개 PASS |
| `make -j2 test-tsan` | 테스트 프로그램 16개 PASS |
| `make MODE=asan run` | native LeakSanitizer fatal error로 실패, `make`도 실패 반환 |

README의 C++ 사용 예제도 Clang으로 별도 컴파일하고 실행했습니다.
LeakSanitizer가 출력한 ptrace 안내 문구만으로 실제 ptrace 간섭이 확인된 것은 아니며,
native 실패 시 원본 stderr를 확인해야 합니다.

이 브랜치는 수락된 제품 헤더와 기존 테스트를 그대로 사용하면서 README, Makefile 및 실행 예제를 추가합니다.
Controller의 상태·동결 기록·보호 검사·수락 기준은 별도의 운영 도구에서 관리합니다.

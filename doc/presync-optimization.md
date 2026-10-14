# 프리싱크 체인워크 계산 최적화 결과

2026-10-14. 먼저 구현·반복 측정·일반 안전성 검사를 진행한 뒤 결과를 작성했다.

## 적용한 변경

`src/headerssync.cpp`의 PRESYNC와 REDOWNLOAD 체인워크 누적에 기존 `GetBitsProof(bits, true)` 정확한 계산 경로를 재사용했다. 적합한 compact target은 64비트 계산을 사용하고 나머지는 기존 256비트 계산으로 돌아간다. 이 함수의 `fast_startup` 인자 이름은 계산 경로 선택용이며, 이번 호출은 운영 `-fast-startup` 설정이나 과거 PoW 신뢰 정책을 바꾸지 않는다.

Yespower 계산, 워커 8개, 실패 판단 전 최대 8개 cold 작업, 난이도·MTP·연결·commitment·최소 체인워크 조건은 그대로다. 추가 캐시나 메모리 할당은 도입하지 않았다. RSS 5GiB를 보장하는 전역 제한을 추가한 것은 아니다.

## 실측

현재 HEAD a63275b21c를 기준으로 같은 빌드의 baseline/candidate/candidate/baseline 순서로 비교했다. 실제 메인넷 헤더 6,000개, PRESYNC+REDOWNLOAD 300라운드이며 genuine PoW 입력 확인은 타이머 밖에서 실행한다. 매 라운드 상태 전환과 반환 헤더를 확인한다.

| 실행 | 변경 전 | 변경 후 |
|---|---:|---:|
| 첫 측정 | 5.08020초 | 4.21122초 |
| 두 번째 측정 | 5.12126초 | 4.16071초 |
| 중앙값 | 5.10073초 | 4.185965초 |

상태 머신 구성요소 시간은 **17.934% 감소**, 처리량은 약 **21.85% 증가**했다. 이것은 실제 Yespower 계산과 네트워크를 포함한 전체 프리싱크 성능이 아니다. 전체 7시간에 이 비율을 적용해서는 안 된다.

별도 8워커 proof 측정은 최초 6,000개 2.98655초(2,009.01개/초, 실제 Yespower 6,000회), 재검증 0.00653611초(실제 Yespower 0회)였다. 운영 노드가 계속 CPU를 사용하는 상황이므로 독점 머신의 처리량 상한이나 개선 전후 비교가 아니다. 캐시가 이미 재사용되고 있다는 사실은 확인한다.

## 기대 효과와 한계

채택 가능한 작은 계산 최적화지만 7시간을 3시간으로 줄이는 해법을 확보한 것은 아니다. 최초 Yespower 비용이 훨씬 크며, 이번 변경의 전체 단축률은 아직 미측정이다. 3시간을 목표로 검증을 약화하거나 워커를 8개 이상으로 늘리지 않았다.

캐시 확대·작업 큐 재설계는 이번 커밋에 포함하지 않았다. 이를 적용하려면 실제 중복 PoW 계산과 워커 대기 비용의 추가 근거가 필요하다. 이미 구현된 공유 캐시·rolling target sum·SugarShield 나눗셈 최적화를 새 성과로 계산하지 않았다.

## 검증

- 일반 chain/pow/header-sync: 21개, 6,872,266 assertions 통과.
- 일반 SugarShield: 12개, 404,609 assertions 통과. 실제 메인넷 6,000개 검증과 양 단계 왕복 포함.
- 독립 256비트 기준식과 compact 지수·부호·overflow·경계값 비교를 포함한 기존 exact-arithmetic 검사 통과.
- 격리 `p2p_headers_sync_with_minchainwork.py` 통과.
- 격리 `p2p_sugarchain_header_pow.py` 통과.
- daemon·GUI·CLI·unit 빌드 완료.
- ASan + UBSan + 누수 검사: 같은 33개, 7,074,267 assertions 통과. 일반 검사와 assertion 수 차이는 무작위 chain fixture 크기에 따른다.

운영 노드와 데이터는 재시작·변경하지 않았다. 빌드 결과는 다음 사용자 재시작부터 적용되며, 프리싱크 도중 재시작하면 진행을 잃을 수 있으므로 이 작은 변경을 적용하려고 지금 재시작하는 것은 권하지 않는다.

## PR 분리

PR #11은 임시 통합 상태다. 최종적으로 다음 세 PR로 분리해 각각 검토·측정한다.

1. Lazy PoW: 백그라운드 과거 검증·검증 결과 저장과 재사용·병렬화.
2. RSS 최적화: IBD 메모리 사용과 순간 피크 감소.
3. 프리싱크 속도 최적화: 이번 체인워크 계산 변경을 포함한 시간 단축.

주목적이 프리싱크 시간 단축이면 메모리 사용을 조정하더라도 세 번째 범위로 분류한다. 실제 PR 분리·병합은 이번에 수행하지 않았다.

## 재현 및 근거

```bash
build/bin/bench_headers_sync src/test/data/sugarchain_headers.raw 6000 300
build/bin/bench_header_pow src/test/data/sugarchain_headers.raw 6000 8 16
```

입력 6,000헤더 SHA256: `d0560434d8927def1b9a656822b74715cc030440877c18735d8256c6d0e0c67b`.

원시 결과·빌드·검사 로그: `/home/ak/.local/state/sugarchain-komorebi/presync-speed/`.
기존 성능 자료: `doc/ibd-optimization.md`.

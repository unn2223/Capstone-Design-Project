# EV Wireless Charging Platform

<img width="300" height="260" alt="image" src="https://github.com/user-attachments/assets/ad04128c-f23c-420d-82ea-64bb2cb06cc8" />
<img width="300" height="260" alt="image" src="https://github.com/user-attachments/assets/bff8342a-64a8-4578-b123-b1be6f4dfffc" />
<img width="480" height="260" alt="image" src="https://github.com/user-attachments/assets/46b7581c-7e9d-4b93-8358-20ee4098c561" />


FastAPI 기반 백엔드, 두 개의 Vite/React 프런트엔드(user/admin), 카메라 캡처 워커, 아두이노 펌웨어로 구성된 무선 충전 데모 시스템입니다. 번호판 인식(GPT 또는 외부 HTTP 서비스), 예약/취소, 실시간 대시보드, 배터리 모니터링(Firebase RTDB)까지 한 번에 확인할 수 있습니다.

## 저장소 구성
- `backend/` : FastAPI + SQLAlchemy API 서버. 기본 SQLite DB(`data/ev_charging.db`) 사용.
- `user-front/` : 사용자 예약 UI (React, Vite, Tailwind).
- `admin-front/` : 운영자 대시보드 UI (React, Vite, Tailwind).
- `camera-capture/` : Firebase 신호를 받아 OpenCV로 촬영 → 번호판 인식 → 백엔드 매칭 → 보고서 저장(+선택적 시리얼 트리거).
- `total_system/`, `mega_code/` : XY 스테이지/충전 제어용 아두이노 스케치.
- 스크립트: `run.ps1`(로컬 올인원 실행), `seed_sessions.py`(충전 세션 시드) 등.

## 백엔드(FastAPI)
### 빠른 시작
1. `python -m venv .venv && .\.venv\Scripts\activate`
2. `pip install -r backend/requirements.txt`
3. 환경 변수 설정(아래 참고) 후 서버 실행  
   `uvicorn backend.app:app --reload --host 0.0.0.0 --port 8000`

### 주요 환경 변수
- `DATABASE_URL` (default `sqlite:///./data/ev_charging.db`)
- `BUSINESS_TIMEZONE` (default `Asia/Seoul`)
- `ADMIN_EMAIL`, `ADMIN_PASSWORD`, `ADMIN_TOKEN` (default: `admin@demo.dev` / `admin123` / `admin-demo-token`)
- `AUTO_SEED_SESSIONS` (기본 0; 1/true/on 시 시작 시 세션 ID 1~4 자동 생성)
- `CORS_ORIGINS` (콤마 구분, 예: `http://localhost:5173,http://localhost:5174`)
- 번호판 인식
  - `PLATE_SERVICE_MODE` `gptapi`(기본) 또는 `http`
  - `OPENAI_API_KEY`, `PLATE_OPENAI_MODEL`(기본 `gpt-5-mini`), `PLATE_OPENAI_PROMPT`
  - `PLATE_SERVICE_URL` (`http` 모드 시 업스트림 인식 엔드포인트)
- 배터리 모니터링(Firebase RTDB)
  - `BATTERY_DATABASE_URL`, `BATTERY_DATABASE_PATH`(기본 `/car-battery-now`), `BATTERY_DATABASE_AUTH`
- 추가: `CORS_ORIGINS`, `PLATE_SERVICE_ENDPOINT`(동일 의미), `PLATE_OPENAI_*` 설정. `OPENAI_API_KEY`가 비어 있으면 루트의 키 파일을 자동으로 읽으려 시도합니다.

### 데이터 모델·동작
- 테이블: `charging_sessions`, `reservations`, `reservation_slots`. 예약은 세션/시작시각 유니크, 슬롯은 30분 단위로 생성·중복 검사.
- 예약 상태는 저장된 값과 현재 시각을 기반으로 `CONFIRMED/IN_PROGRESS/COMPLETED/CANCELLED`를 파생.
- 시간은 비즈니스 타임존을 로컬로 받아 UTC로 저장·비교하며, 24:00 허용, 30분 단위만 생성 가능. 같은 차량(번호판) 시간 겹침/세션 겹침은 거부.
- 시작 시 DB 생성/마이그레이션(예약 UTC 보정, 슬롯 보강, `contact_email` 컬럼 추가) 후 필요 시 세션 자동 시드.

### 주요 API
- 시스템: `GET /health`
- 공개 예약
  - `GET /api/sessions` : 세션/예약 전체 목록
  - `GET /api/reservations/by-session?date=YYYY-MM-DD`
  - `POST /api/reservations` : 단건 예약 생성
  - `POST /api/reservations/batch` : 여러 시작 시각(각 60분) 일괄 예약
  - `GET /api/reservations/my?email=...&plate=...` : 사용자 본인 조회
  - `DELETE /api/reservations/{id}?email=...&plate=...`
- 번호판/매칭
  - `POST /api/plates/verify` : 특정 시간대 충돌 여부 사전 검증
  - `POST /api/plates/match` : `{plate, timestamp}`로 활성 예약 매칭
  - `POST /api/license-plates` (구 `/api/plates/recognize`) : 이미지 업로드 → 인식(GPT 또는 HTTP 프록시)
- 인증
  - `POST /api/user/login` : 단순 토큰 발급(데모용)
  - `POST /api/admin/login` : 운영자 로그인
  - `GET /api/admin/reservations/by-session?date=...`, `DELETE /api/admin/reservations/{id}`
- 배터리: `GET /api/battery/now` : Firebase RTDB에서 최신 퍼센트/전압 조회

## 프런트엔드
공통: `npm install` 후 `npm run dev -- --host --port <포트>` (추천: user 5173, admin 5174). `VITE_API_BASE`로 백엔드 URL 지정(미설정 시 호스트 기준 `:8000` 사용). `npm run build`로 정적 빌드 가능.

### user-front
- 흐름: 로그인(이메일/패스워드 임의) → 번호판 입력/카메라 캡처 → 예약 가능 여부 확인 → 시간/세션 선택(단일 또는 60분 단위 다중) → 예약 생성 → 결과/내 예약 조회 및 취소.
- 기능: 번호판 정규식 검증, 충돌 사전 검사, 30분 단위 슬롯 겹침 막기, 다일/열 지도형 가용도 표시, 가격 견적, 카메라 미디어 스트림 캡처 후 백엔드 인식 API 호출, 내 예약 상세(배터리 상태 포함), 전체 삭제 지원.

### admin-front
- 기본 자격 증명: `ADMIN_EMAIL` / `ADMIN_PASSWORD`(기본 `admin@demo.dev` / `admin123`), 헤더 토큰은 `ADMIN_TOKEN`.
- 기능: 날짜별 세션별 예약 그리드/리스트, KPI 카드(총 예약/진행 중/가용률), 15초 자동 새로고침 토글, 예약 삭제.

## 카메라 캡처 워커
- 동작: Firebase RTDB 신호(`--signal-path`, 기본 `/signals/car_on_parkinglot`) 감지 → OpenCV 촬영 → 번호판 인식 HTTP 호출(`--recognition-url`, 기본 백엔드 `/api/license-plates`) → 백엔드 매칭(`--match-url`, 기본 `/api/plates/match`) → 결과를 RTDB(match path)와 JSON 리포트(`camera-capture/reports/*.json`)에 기록. 선택적으로 Firebase Storage 업로드와 시리얼 포트 트리거 전송.
- 핵심 옵션/환경: `FIREBASE_CREDENTIALS`(서비스 계정 JSON), `FIREBASE_DATABASE_URL`, `FIREBASE_STORAGE_BUCKET`, `FIREBASE_SIGNAL_PATH`, `FIREBASE_TIMESTAMP_PATH`, `PLATE_SERVICE_URL`, `PLATE_MATCH_URL`, `CAMERA_INDEX`/`CAMERA_NAME_HINT`, `PIPELINE_MODE`(`gpt|storage|both`), `AUTO_UPLOAD_TO_STORAGE`, `PLATE_MATCH_SERIAL_*`. `--skip-firebase`로 로컬 테스트 가능, `--list-cameras`로 DirectShow 장치 조회.
- 예시(로컬 테스트):  
  `python camera-capture/main.py --skip-firebase --pipeline-mode gpt --recognition-url http://localhost:8000/api/license-plates --match-url http://localhost:8000/api/plates/match`

## 하드웨어 스케치
- `total_system/total_system.ino`, `mega_code/mega_code.ino` : 초음파 센서로 차량 위치를 잡아 XY 서보 정렬 후 전류 센서 기반 충전 시나리오를 실행하는 상태 머신 구현. LCD 표시, LED 상태, 시리얼 입력으로 목표 용량 설정/번호판 수신을 지원.

## 유용한 스크립트
- `run.ps1` : 가상환경/의존성 확인 후 백엔드(uvicorn)와 두 프런트엔드를 동시에 실행, 기본 CORS/VITE_API_BASE 설정, 필요 시 카메라 워커(`RUN_CAMERA_WORKER=1`, `CAMERA_WORKER_ARGS`)도 구동.
- `seed_sessions.py` : 세션 수를 지정해 시드(`python backend/seed_sessions.py --count 4`).

## 권장 개발 흐름
1. 백엔드 실행(`uvicorn ...`, CORS/OPENAI/RTDB 환경 설정).
2. 사용자 UI `npm run dev -- --host --port 5173`, 운영자 UI는 다른 포트에서 실행.
3. 번호판 인식이 필요하면 `OPENAI_API_KEY` 또는 외부 인식 서비스/카메라 워커 설정.
4. Firebase를 쓴다면 RTDB URL/인증/Storage 버킷을 맞추고 워커를 띄운 뒤 매칭 결과를 확인합니다.

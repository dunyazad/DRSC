import os
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D

# ==========================================
# [설정] 여기에 실제 로그 파일 이름을 적으세요
# ==========================================
log_filename = "match_1_log.txt" 

print(f"--- 파일 확인 시작: {log_filename} ---")

# 1. 파일 존재 여부 확인
if not os.path.exists(log_filename):
    print(f"[오류] '{log_filename}' 파일이 현재 폴더에 없습니다.")
    print(f"현재 파이썬 실행 위치: {os.getcwd()}")
    # 파일이 다른 폴더에 있다면 절대 경로를 적어주세요.
else:
    print(f"[성공] 파일을 찾았습니다.")

    # 2. 파일 내용 미리보기 (앞 5줄만)
    try:
        with open(log_filename, 'r', encoding='utf-8') as f:
            lines = f.readlines()
            if not lines:
                print("[경고] 파일이 비어 있습니다. (C++ 서버가 종료되었나요? logFile.close()가 호출되었는지 확인하세요)")
            else:
                print(f"[정보] 총 {len(lines)} 줄의 데이터가 있습니다.")
                print("--- 내용 미리보기 (상위 5줄) ---")
                for i, line in enumerate(lines[:5]):
                    print(f"{i+1}: {line.strip()}")
    except Exception as e:
        print(f"[오류] 파일을 읽는 중 에러 발생: {e}")

print("--- 확인 종료 ---")

def parse_and_plot(log_filename):
    data = {} # { player_index: {'t': [], 'x': [], 'y': [], 'z': []} }

    try:
        with open(log_filename, 'r') as f:
            print(f"Reading file: {log_filename}...")
            
            for line in f:
                parts = line.strip().split('|')
                
                # 데이터가 온전치 않으면 건너뜀
                if len(parts) < 3:
                    continue
                    
                try:
                    timestamp = int(parts[0])
                    msg_type = parts[1]
                    payload = parts[2]
                except ValueError:
                    continue
                
                # "PLAYER_TRANSFORM_UPDATE" 메시지만 처리
                if msg_type == "PLAYER_TRANSFORM_UPDATE":
                    # Payload 예시: "0,100,200,300,0,0,0/1,150,250,350,0,0,0"
                    players = payload.split('/')
                    
                    for p_str in players:
                        vals = p_str.split(',')
                        # 데이터 개수 체크 (Index, X, Y, Z, R, P, Y -> 최소 7개)
                        if len(vals) < 7: continue
                        
                        try:
                            p_idx = int(vals[0])
                            x = float(vals[1])
                            y = float(vals[2])
                            z = float(vals[3])
                            
                            if p_idx not in data:
                                data[p_idx] = {'t': [], 'x': [], 'y': [], 'z': []}
                                
                            data[p_idx]['t'].append(timestamp)
                            data[p_idx]['x'].append(x)
                            data[p_idx]['y'].append(y)
                            data[p_idx]['z'].append(z)
                        except ValueError:
                            print('Error')
                            continue

    except FileNotFoundError:
        print(f"Error: File '{log_filename}' not found.")
        print("Make sure the C++ server has finished running and the file exists.")
        return

    # 데이터가 없으면 종료
    if not data:
        print("No 'PLAYER_TRANSFORM_UPDATE' data found in the file.")
        return

    print(f"Found data for {len(data)} player(s). Plotting...")

    # 3D 그래프 그리기
    fig = plt.figure(figsize=(10, 8))
    ax = fig.add_subplot(111, projection='3d')

    for p_idx, coords in data.items():
        # 선 그리기
        ax.plot(coords['x'], coords['y'], coords['z'], label=f'Player {p_idx}')
        
        # 시작점(초록색 동그라미)과 끝점(빨간색 X) 표시
        if coords['x']:
            ax.scatter(coords['x'][0], coords['y'][0], coords['z'][0], c='green', marker='o', s=50)
            ax.scatter(coords['x'][-1], coords['y'][-1], coords['z'][-1], c='red', marker='x', s=50)

    ax.set_title(f'Drone Trajectory: {log_filename}')
    ax.set_xlabel('X')
    ax.set_ylabel('Y')
    ax.set_zlabel('Z')
    ax.legend()
    
    plt.show()

if __name__ == "__main__":
    parse_and_plot(log_filename)
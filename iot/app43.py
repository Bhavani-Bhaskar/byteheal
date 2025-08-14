from flask import Flask, Response, render_template_string, request, json, send_from_directory
import cv2
import mediapipe as mp
import threading
import time
import os
import pandas as pd
import numpy as np
import random
from gtts import gTTS
import pygame
import io
import base64

app = Flask(__name__)

# Initialize pygame mixer for audio playback
pygame.mixer.init()

# Load the pose reference data
with open('medianreference.json') as f:
    POSE_REFERENCE = json.load(f)

# Get a list of all pose names
POSE_NAMES = list(POSE_REFERENCE.keys())

# Pose images directory
POSE_IMAGE_DIR = "idealposes"
os.makedirs(POSE_IMAGE_DIR, exist_ok=True)

# Pose descriptions and instructions
POSE_DESCRIPTIONS = {}
try:
    with open('pose_description.json') as f:
        POSE_DESCRIPTIONS = json.load(f)
except FileNotFoundError:
    print("Warning: pose_description.json not found. Using empty descriptions.")
    POSE_DESCRIPTIONS = {name: {"description": "", "instructions": ""} for name in POSE_NAMES}

# ── MediaPipe setup ──────────────────────────────────────────────────────────────────────────────────
mp_drawing = mp.solutions.drawing_utils
mp_pose = mp.solutions.pose
pose = mp_pose.Pose(
    static_image_mode=False,
    model_complexity=1,
    smooth_landmarks=True,
    enable_segmentation=False,
    min_detection_confidence=0.5,
    min_tracking_confidence=0.5
)

# ── Globals ──────────────────────────────────────────────────────────────────────────────────────────
latest_frame = None
latest_landmarks = None
current_angles = None
lock = threading.Lock()
current_pose = None
timer_active = False
remaining_time = 0
pose_history = []
audio_playing = False
modal_visible = True
tts_thread = None
all_angles_correct = False
angle_statuses = {}  # Track individual angle correctness

# ── Angles definition ────────────────────────────────────────────────────────────────────────────────
YOGA_ANGLES = {
    "right-hand":           [16, 14, 12], 
    "left-hand":            [15, 13, 11],
    "right-arm-body":       [14, 12, 24],
    "left-arm-body":        [13, 11, 23],
    "right-leg-body":       [12, 24, 26],
    "left-leg-body":        [11, 23, 25],
    "right-leg":            [24, 26, 28],
    "left-leg":             [23, 25, 27]
}

# ── Helper Functions ─────────────────────────────────────────────────────────────────────────────────
def calculate_angle(a, b, c):
    a = np.array(a)
    b = np.array(b)
    c = np.array(c)
    radians = np.arctan2(c[1]-b[1], c[0]-b[0]) - np.arctan2(a[1]-b[1], a[0]-b[0])
    angle = abs(radians * 180.0 / np.pi)
    if angle > 180.0:
        angle = 360.0 - angle
    return round(angle, 2)

def calc_yoga_angles(landmarks):
    ang = {}
    for name, (p1, p2, p3) in YOGA_ANGLES.items():
        vis = [landmarks[i]["visibility"] for i in (p1, p2, p3)]
        if min(vis) < 0.5:
            ang[name] = None
        else:
            a = (landmarks[p1]['x'], landmarks[p1]['y'])
            b = (landmarks[p2]['x'], landmarks[p2]['y'])
            c = (landmarks[p3]['x'], landmarks[p3]['y'])
            ang[name] = calculate_angle(a, b, c)
    return ang

def check_individual_angles_and_feedback(current_angles, current_pose):
    """Optimized combined function that checks angles and generates feedback in a single pass
    
    Returns:
        tuple: (angle_statuses, feedback_text)
            angle_statuses: dict of angle_name -> bool (True if angle is correct)
            feedback_text: string with adjustment instructions
    """
    if current_pose not in POSE_REFERENCE or not current_angles:
        return (
            {name: False for name in YOGA_ANGLES.keys()},
            "No pose reference data available" if current_pose not in POSE_REFERENCE 
            else "No angles detected"
        )
    
    pose_ref = POSE_REFERENCE[current_pose]
    angle_statuses = {}
    feedback_parts = []
    
    for angle_name, current_value in current_angles.items():
        # Initialize as incorrect by default
        angle_statuses[angle_name] = False
        
        if current_value is None:
            feedback_parts.append(f"Cannot detect {angle_name.replace('-', ' ')}. Make sure it's visible.")
            continue
            
        # Get the reference data for this angle
        ref_name = angle_name.replace('-', '_')
        ref_data = pose_ref.get(ref_name)
        if not ref_data:
            continue
            
        median = ref_data['median']
        iqr = ref_data['iqr']
        lower_bound = median - iqr
        upper_bound = median + iqr
        
        # Check angle status and generate feedback if needed
        if lower_bound <= current_value <= upper_bound:
            angle_statuses[angle_name] = True
        else:
            if current_value < lower_bound:
                feedback_parts.append(f"Extend your {angle_name.replace('-', ' ')} more.")
            else:
                feedback_parts.append(f"Contract your {angle_name.replace('-', ' ')} slightly.")
    
    return angle_statuses, " ".join(feedback_parts)

# Wrapper functions to maintain backward compatibility
def check_individual_angles(current_angles, current_pose):
    """Wrapper for the old function signature"""
    angle_statuses, _ = check_individual_angles_and_feedback(current_angles, current_pose)
    return angle_statuses

def get_angle_feedback(current_angles, current_pose):
    """Wrapper for the old function signature"""
    _, feedback = check_individual_angles_and_feedback(current_angles, current_pose)
    return feedback

def check_angles_within_threshold(current_angles, current_pose):
    """Check if all angles are within the median±iqr range"""
    if current_pose not in POSE_REFERENCE or not current_angles:
        return False
    
    # Use optimized function and check if all angles are correct
    individual_statuses, _ = check_individual_angles_and_feedback(current_angles, current_pose)
    return all(status for status in individual_statuses.values())

def text_to_speech(text):
    """Convert text to speech and play it"""
    global audio_playing, modal_visible
    
    try:
        tts = gTTS(text=text, lang='en')
        audio_file = io.BytesIO()
        tts.write_to_fp(audio_file)
        audio_file.seek(0)
        
        pygame.mixer.music.load(audio_file)
        pygame.mixer.music.play()
        audio_playing = True
        
        # Wait for the audio to finish playing
        while pygame.mixer.music.get_busy() and audio_playing:
            time.sleep(0.1)
            
    except Exception as e:
        print(f"Error in TTS: {e}")
    finally:
        audio_playing = False

def get_next_pose():
    """Get the next pose to display, ensuring we don't repeat until all are shown"""
    global pose_history, POSE_NAMES
    
    if len(pose_history) == len(POSE_NAMES):
        pose_history = []
    
    available_poses = [p for p in POSE_NAMES if p not in pose_history]
    
    if not available_poses:
        return random.choice(POSE_NAMES)
    
    next_pose = random.choice(available_poses)
    pose_history.append(next_pose)
    return next_pose

def start_timer(duration=30):
    """Start the timer for the current pose"""
    global timer_active, remaining_time, modal_visible, all_angles_correct
    
    # Only start timer if all angles are correct
    with lock:
        if current_angles and current_pose:
            all_angles_correct = check_angles_within_threshold(current_angles, current_pose)
    
    if not all_angles_correct:
        # Give feedback instead of starting timer
        feedback = get_angle_feedback(current_angles, current_pose)
        if feedback and not audio_playing:
            threading.Thread(target=text_to_speech, args=(feedback,), daemon=True).start()
        return
    
    # Hide modal when timer starts
    modal_visible = False
    
    timer_active = True
    remaining_time = duration
    
    def timer_countdown():
        global timer_active, remaining_time, modal_visible, all_angles_correct
    
        while remaining_time > 0 and timer_active:
            time.sleep(1)
            remaining_time -= 1
        
            # Check angles periodically during timer
            with lock:
                if current_angles and current_pose:
                    all_angles_correct = check_angles_within_threshold(current_angles, current_pose)
        
            # If angles go out of range, pause timer and give feedback
            if not all_angles_correct:
                feedback = get_angle_feedback(current_angles, current_pose)
                if feedback and not audio_playing:
                    threading.Thread(target=text_to_speech, args=(feedback,), daemon=True).start()
                timer_active = False
                return
            time.sleep(0.5)

    threading.Thread(target=timer_countdown, daemon=True).start()

def set_current_pose(pose_name):
    """Set the current pose and trigger TTS and modal"""
    global current_pose, timer_active, modal_visible, tts_thread, all_angles_correct, angle_statuses
    
    # Stop any ongoing timer and audio
    timer_active = False
    if audio_playing:
        pygame.mixer.music.stop()
    
    # Set new pose and show modal
    current_pose = pose_name
    modal_visible = True
    all_angles_correct = False
    angle_statuses = {name: False for name in YOGA_ANGLES.keys()}
    
    # Get pose description
    pose_data = POSE_DESCRIPTIONS.get(pose_name, {})
    description = pose_data.get("description", "")
    instructions = pose_data.get("instructions", "")
    
    # Combine for TTS
    tts_text = f"{pose_name}. {description} {instructions}"
    
    # Play TTS in a separate thread
    if tts_thread and tts_thread.is_alive():
        tts_thread.join()
    
    tts_thread = threading.Thread(target=text_to_speech, args=(tts_text,), daemon=True)
    tts_thread.start()

# ── Webcam capture thread ───────────────────────────────────────────────────────────────────────────
def capture_loop():
    global latest_frame, latest_landmarks, current_angles, all_angles_correct, angle_statuses
    cap = cv2.VideoCapture(0, cv2.CAP_DSHOW)
    if not cap.isOpened():
        raise RuntimeError("Cannot open webcam")
    while True:
        ret, frame = cap.read()
        if not ret:
            continue
        frame = cv2.flip(frame, 1)
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        results = pose.process(rgb)
        with lock:
            latest_frame = frame.copy()
            if results.pose_landmarks:
                latest_landmarks = [
                    {"id": idx, "x": lm.x, "y": lm.y, "visibility": lm.visibility}
                    for idx, lm in enumerate(results.pose_landmarks.landmark)
                ]
                current_angles = calc_yoga_angles(latest_landmarks)
                
            # Check individual angle statuses and overall correctness
                if current_pose:
                    angle_statuses, _ = check_individual_angles_and_feedback(current_angles, current_pose)
                    all_angles_correct = all(status for status in angle_statuses.values())
                
                # Draw landmarks
                mp_drawing.draw_landmarks(
                    latest_frame, results.pose_landmarks, mp_pose.POSE_CONNECTIONS,
                    mp_drawing.DrawingSpec(color=(0,255,0), thickness=2, circle_radius=2),
                    mp_drawing.DrawingSpec(color=(0,0,255), thickness=2)
                )
        time.sleep(0.01)

threading.Thread(target=capture_loop, daemon=True).start()

# ── HTML Templates ──────────────────────────────────────────────────────────────────────────────────
HOME_PAGE = """
<!doctype html><title>Yoga Guide</title>
<style>
    body {
        font-family: Arial, sans-serif;
        text-align: center;
        padding: 50px;
        background-color: #f5f5f5;
    }
    .quote {
        font-size: 24px;
        font-style: italic;
        margin: 40px 0;
        color: #333;
    }
    .start-btn {
        padding: 15px 30px;
        font-size: 18px;
        background-color: #4CAF50;
        color: white;
        border: none;
        border-radius: 5px;
        cursor: pointer;
        transition: background-color 0.3s;
    }
    .start-btn:hover {
        background-color: #45a049;
    }
</style>

<h1>Welcome to Yoga Posture Guide</h1>
<div class="quote">
    "Yoga is the journey of the self, through the self, to the self."<br>
    - The Bhagavad Gita
</div>
<button class="start-btn" onclick="window.location.href='/app'">Start Practice</button>
"""

APP_PAGE = """
<!doctype html><title>Yoga Practice</title>
<style>
    body {
        font-family: Arial, sans-serif;
        margin: 0;
        padding: 0;
        background-color: #f5f5f5;
        height: 100vh;
        overflow: hidden;
    }
    .container {
        display: flex;
        flex-direction: row;
        height: 100vh;
        gap: 20px;
    }
    .video-container {
        position: relative;
        flex: 1;
        height: 100%;
    }
    #yoga_feed {
        width: 100%;
        height: 100%;
        object-fit: cover;
    }
    .dashboard {
        width: 320px;
        padding: 15px;
        background: #fff;
        border-radius: 8px;
        box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        height: calc(100vh - 40px);
        overflow-y: auto;
    }
    .angle-meter {
        margin-bottom: 15px;
        padding: 10px;
        border-radius: 5px;
        background: #f9f9f9;
    }
    .angle-name {
        font-weight: bold;
        margin-bottom: 5px;
        display: flex;
        justify-content: space-between;
        align-items: center;
    }
    .angle-status {
        font-size: 16px;
        font-weight: bold;
    }
    .meter-container {
        position: relative;
        width: 100%;
        height: 25px;
        background: #e0e0e0;
        border-radius: 12px;
        overflow: hidden;
        margin: 5px 0;
    }
    .threshold-range {
        position: absolute;
        height: 100%;
        background-color: rgba(76, 175, 80, 0.3);
        border-left: 2px solid #4CAF50;
        border-right: 2px solid #4CAF50;
        z-index: 1;
    }
    .meter-bar {
        position: relative;
        z-index: 2;
        height: 100%;
        width: 0%;
        transition: all 0.3s ease;
        border-radius: 12px;
    }
    .angle-value {
        text-align: right;
        font-size: 12px;
        margin-top: 3px;
        display: flex;
        justify-content: space-between;
    }
    .pose-image {
        position: absolute;
        top: 10px;
        right: 10px;
        width: 150px;
        border: 2px solid #4CAF50;
        border-radius: 5px;
        z-index: 100;
    }
    .timer {
        position: absolute;
        top: 10px;
        left: 10px;
        background: rgba(0,0,0,0.7);
        color: white;
        padding: 5px 10px;
        border-radius: 5px;
        font-size: 18px;
        z-index: 100;
    }
    .status-indicator {
        position: absolute;
        top: 50px;
        left: 10px;
        color: white;
        padding: 5px 10px;
        border-radius: 5px;
        font-size: 16px;
        font-weight: bold;
        z-index: 100;
        transition: all 0.3s ease;
    }
    .status-correct {
        background-color: #4CAF50 !important;
    }
    .status-incorrect {
        background-color: #f44336 !important;
    }
    .modal {
        display: none;
        position: fixed;
        z-index: 1000;
        left: 0;
        top: 0;
        width: 100%;
        height: 100%;
        background-color: rgba(0,0,0,0.8);
    }
    .modal.visible {
        display: block;
    }
    .modal-content {
        background-color: #fefefe;
        margin: 5% auto;
        padding: 20px;
        border: 1px solid #888;
        width: 80%;
        max-height: 80vh;
        overflow-y: auto;
        border-radius: 8px;
    }
    .close {
        color: #aaa;
        float: right;
        font-size: 28px;
        font-weight: bold;
        cursor: pointer;
    }
    .close:hover {
        color: black;
    }
    .pose-info {
        display: flex;
        gap: 20px;
        margin-bottom: 20px;
    }
    .pose-info img {
        max-width: 300px;
        border-radius: 5px;
    }
    .pose-details {
        flex: 1;
    }
    .continue-btn {
        padding: 10px 20px;
        background-color: #4CAF50;
        color: white;
        border: none;
        border-radius: 5px;
        cursor: pointer;
        font-size: 16px;
        margin-top: 20px;
    }
    .continue-btn:hover {
        background-color: #45a049;
    }
    .progress-bar {
        width: 100%;
        height: 10px;
        background-color: #e0e0e0;
        border-radius: 5px;
        margin-top: 10px;
        overflow: hidden;
    }
    .progress {
        height: 100%;
        width: 0%;
        transition: width 1s linear;
        background-color: #4CAF50;
    }
    .feedback-message {
        position: absolute;
        bottom: 20px;
        left: 50%;
        transform: translateX(-50%);
        background: rgba(0,0,0,0.7);
        color: white;
        padding: 10px 20px;
        border-radius: 5px;
        font-size: 16px;
        z-index: 100;
        max-width: 80%;
        text-align: center;
        display: none;
    }
</style>

<div id="poseModal" class="modal">
    <div class="modal-content">
        <span class="close" onclick="closeModal()">&times;</span>
        <h2 id="poseTitle"></h2>
        <div id="poseContent" class="pose-info">
            <!-- Pose info will be inserted here -->
        </div>
        <div class="progress-bar">
            <div class="progress" id="poseProgress"></div>
        </div>
        <button class="continue-btn" onclick="closeModal()">Continue to Practice</button>
    </div>
</div>

<div class="container">
    <div class="video-container">
        <div class="timer" id="timerDisplay">30s</div>
        <div class="status-indicator status-incorrect" id="statusIndicator">Adjust your pose</div>
        <div class="feedback-message" id="feedbackMessage"></div>
        <img id="poseImage" class="pose-image" src="" alt="Target Pose"/>
        <img id="yoga_feed" src="/video_feed" width="800" height="1000"/>
    </div>
    
    <div class="dashboard">
        <h3>Angle Dashboard</h3>
        <div id="angle-container">
            <!-- Angle meters will be inserted here by JavaScript -->
        </div>
    </div>
</div>

<script>
// Create angle meters for each angle
const angleNames = [
    "right-hand", "left-hand", 
    "right-arm-body", "left-arm-body",
    "right-leg-body", "left-leg-body",
    "right-leg", "left-leg"
];

// Initialize dashboard
function initDashboard() {
    const container = document.getElementById('angle-container');
    angleNames.forEach(name => {
        const meter = document.createElement('div');
        meter.className = 'angle-meter';
        meter.innerHTML = `
            <div class="angle-name">
                <span>${name.replace(/-/g, ' ')}</span>
                <span id="status-${name}" class="angle-status">✗</span>
            </div>
            <div class="meter-container">
                <div class="threshold-range" id="range-${name}"></div>
                <div class="meter-bar" id="meter-${name}" style="background-color: #f44336;"></div>
            </div>
            <div class="angle-value">
                <span id="value-${name}">0°</span>
                <span id="threshold-${name}"></span>
            </div>
        `;
        container.appendChild(meter);
    });
}

// Update dashboard with new angle data
function updateDashboard(angles, angleStatuses, thresholds) {
    let overallCorrect = true;
    
    angleNames.forEach(name => {
        const value = angles[name];
        const isCorrect = angleStatuses[name] || false;
        const meter = document.getElementById(`meter-${name}`);
        const range = document.getElementById(`range-${name}`);
        const status = document.getElementById(`status-${name}`);
        
        if (value !== null && value !== undefined) {
            const percent = Math.min((value / 180) * 100, 100);
            meter.style.width = `${percent}%`;
            document.getElementById(`value-${name}`).textContent = `${value}°`;
            
            // Update threshold display if available
            if (thresholds && thresholds[name]) {
                const { lower, upper } = thresholds[name];
                const lowerPercent = Math.max((lower / 180) * 100, 0);
                const upperPercent = Math.min((upper / 180) * 100, 100);
                range.style.left = `${lowerPercent}%`;
                range.style.width = `${Math.max(upperPercent - lowerPercent, 0)}%`;
                document.getElementById(`threshold-${name}`).textContent = `${Math.round(lower)}°-${Math.round(upper)}°`;
            }
            
            // Set color and status based on correctness
            if (isCorrect) {
                meter.style.backgroundColor = '#4CAF50'; // Green when correct
                status.textContent = '✓';
                status.style.color = '#4CAF50';
            } else {
                meter.style.backgroundColor = '#f44336'; // Red when incorrect
                status.textContent = '✗';
                status.style.color = '#f44336';
                overallCorrect = false;
            }
        } else {
            meter.style.width = '0%';
            meter.style.backgroundColor = '#f44336';
            document.getElementById(`value-${name}`).textContent = 'N/A';
            status.textContent = '✗';
            status.style.color = '#f44336';
            overallCorrect = false;
        }
    });
    
    // Update overall status indicator
    const statusEl = document.getElementById('statusIndicator');
    if (statusEl) {
        statusEl.textContent = overallCorrect ? 'Perfect! All angles correct!' : 'Adjust your pose';
        statusEl.className = overallCorrect ? 'status-indicator status-correct' : 'status-indicator status-incorrect';
    }
}

// Update timer display
function updateTimer(seconds) {
    document.getElementById('timerDisplay').textContent = `${seconds}s`;
}

// Update progress bar
function updateProgress(percent) {
    document.getElementById('poseProgress').style.width = `${percent}%`;
}

// Update feedback message
function updateFeedback(message) {
    const feedbackEl = document.getElementById('feedbackMessage');
    if (message && message.trim()) {
        feedbackEl.textContent = message;
        feedbackEl.style.display = 'block';
    } else {
        feedbackEl.style.display = 'none';
    }
}

// Fetch angle data and statuses periodically
function pollAngles() {
    Promise.all([
        fetch('/get_angles').then(r => r.json()),
        fetch('/get_angle_statuses').then(r => r.json()),
        fetch('/get_thresholds').then(r => r.json()),
        fetch('/get_angle_status').then(r => r.json())
    ]).then(([angles, statuses, thresholds, status]) => {
        updateDashboard(angles, statuses, thresholds);
        updateFeedback(status.feedback);
    }).catch(err => console.error('Error fetching angle data:', err))
    .finally(() => {
        setTimeout(pollAngles, 100);
    });
}

// Fetch timer data periodically
function pollTimer() {
    fetch('/get_timer')
        .then(response => response.json())
        .then(data => {
            updateTimer(data.remaining_time);
            updateProgress((30 - data.remaining_time) / 30 * 100);
        }).catch(err => console.error('Error fetching timer:', err))
        .finally(() => {
            setTimeout(pollTimer, 1000);
        });
}

// Fetch modal state and pose info periodically
function pollModalState() {
    fetch('/get_modal_state')
        .then(response => response.json())
        .then(data => {
            const modal = document.getElementById('poseModal');
            if (data.visible) {
                modal.classList.add('visible');
                // Update pose information when modal becomes visible
                fetch('/get_current_pose')
                    .then(response => response.json())
                    .then(poseData => {
                        document.getElementById('poseTitle').textContent = poseData.name;
                        document.getElementById('poseImage').src = poseData.image_url;
                        
                        const poseContent = document.getElementById('poseContent');
                        poseContent.innerHTML = `
                            <img src="${poseData.image_url}" alt="${poseData.name}"/>
                            <div class="pose-details">
                                <h3>Benefits:</h3>
                                <p>${poseData.benefits}</p>
                                <h3>Instructions:</h3>
                                <p>${poseData.instructions}</p>
                            </div>
                        `;
                    }).catch(err => console.error('Error fetching pose data:', err));
            } else {
                modal.classList.remove('visible');
            }
        }).catch(err => console.error('Error fetching modal state:', err))
        .finally(() => {
            setTimeout(pollModalState, 100);
        });
}

// Close the modal (user-initiated)
function closeModal() {
    fetch('/close_modal', { method: 'POST' });
}

// Initialize and start polling
initDashboard();
pollAngles();
pollTimer();
pollModalState();
</script>
"""

# ── Routes ───────────────────────────────────────────────────────────────────────────────────────────
@app.route('/')
def home():
    return render_template_string(HOME_PAGE)

@app.route('/app')
def yoga_app():
    global current_pose
    
    # Select the next pose
    next_pose = get_next_pose()
    set_current_pose(next_pose)
    
    return render_template_string(APP_PAGE)

@app.route('/idealposes/<path:filename>')
def serve_pose_image(filename):
    return send_from_directory(POSE_IMAGE_DIR, filename)

@app.route('/get_current_pose')
def get_current_pose():
    global current_pose
    if not current_pose:
        current_pose = get_next_pose()
    
    pose_data = POSE_DESCRIPTIONS.get(current_pose, {})
    
    # Check for both jpg and png formats
    image_filename_jpg = f"{current_pose}.jpg"
    image_filename_png = f"{current_pose}.png"
    image_path_jpg = os.path.join(POSE_IMAGE_DIR, image_filename_jpg)
    image_path_png = os.path.join(POSE_IMAGE_DIR, image_filename_png)
    
    # Use existing image if found (prefer jpg over png)
    if os.path.exists(image_path_jpg):
        image_url = f"/idealposes/{image_filename_jpg}"
    elif os.path.exists(image_path_png):
        image_url = f"/idealposes/{image_filename_png}"
    else:
        # Create a simple placeholder with pose name
        image_url = f"https://via.placeholder.com/300x200.png?text={current_pose.replace(' ', '+')}"
    
    return {
        "name": current_pose,
        "image_url": image_url,
        "benefits": pose_data.get("description", ""),
        "instructions": pose_data.get("instructions", "")
    }

@app.route('/get_angles')
def get_angles():
    with lock:
        if current_angles is None:
            return {}
        return current_angles

@app.route('/get_angle_statuses')
def get_angle_statuses():
    """Return individual angle correctness status"""
    with lock:
        return angle_statuses

@app.route('/get_angle_status')
def get_angle_status():
    with lock:
        if not current_angles or not current_pose:
            return {
                "all_correct": False,
                "feedback": "Waiting for pose detection...",
                "thresholds": {}
            }
        
        all_correct = check_angles_within_threshold(current_angles, current_pose)
        feedback = "" if all_correct else get_angle_feedback(current_angles, current_pose)
        
        # Get thresholds for current pose
        thresholds = {}
        pose_ref = POSE_REFERENCE.get(current_pose, {})
        for angle_name in YOGA_ANGLES.keys():
            ref_name = angle_name.replace('-', '_')
            if ref_name in pose_ref:
                median = pose_ref[ref_name]['median']
                iqr = pose_ref[ref_name]['iqr']
                thresholds[angle_name] = {
                    'lower': median - iqr,
                    'upper': median + iqr
                }
        
        return {
            "all_correct": all_correct,
            "feedback": feedback,
            "thresholds": thresholds
        }

@app.route('/get_timer')
def get_timer():
    global remaining_time
    return {
        "remaining_time": remaining_time,
        "timer_active": timer_active
    }

@app.route('/get_modal_state')
def get_modal_state():
    global modal_visible
    return {
        "visible": modal_visible
    }

@app.route('/close_modal', methods=['POST'])
def close_modal():
    global audio_playing, modal_visible
    
    # Stop any playing audio
    if audio_playing:
        pygame.mixer.music.stop()
        audio_playing = False
    
    # Hide modal and start timer
    modal_visible = False
    start_timer()
    
    return ('', 204)

@app.route('/get_thresholds')
def get_thresholds():
    global current_pose
    if not current_pose or current_pose not in POSE_REFERENCE:
        return {}
    
    pose_ref = POSE_REFERENCE[current_pose]
    thresholds = {}
    
    for angle_name in YOGA_ANGLES.keys():
        ref_name = angle_name.replace('-', '_')
        if ref_name in pose_ref:
            median = pose_ref[ref_name]['median']
            iqr = pose_ref[ref_name]['iqr']
            thresholds[angle_name] = {
                'lower': median - iqr,
                'upper': median + iqr
            }
    
    return thresholds

def gen_frames():
    global latest_frame
    while True:
        with lock:
            if latest_frame is None:
                continue
            _, buf = cv2.imencode('.jpg', latest_frame)
            frame = buf.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')
        time.sleep(0.05)

@app.route('/video_feed')
def video_feed():
    return Response(gen_frames(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

if __name__ == '__main__':
    app.run(host='127.0.0.1', port=5000, threaded=True)

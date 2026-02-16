// WebSocket and state management
let ws;
let isMatched = false;
let reconnectAttempts = 0;
let maxReconnectAttempts = 5;
let reconnectDelay = 1000; // Start with 1 second
let heartbeatInterval;
let typingTimeout;
let typingIndicator = null;

// Configuration
const serverUrl = 'ws://20.193.154.3:8080'; // Change to your server

// DOM Elements
const messagesContainer = document.getElementById('messagesContainer');
const emptyState = document.getElementById('emptyState');
const messageInput = document.getElementById('messageInput');
const sendBtn = document.getElementById('sendBtn');
const skipBtn = document.getElementById('skipBtn');
const charCount = document.getElementById('charCount');

// ==================== CONNECTION MANAGEMENT ====================

function connect() {
    updateStatus('reconnecting', 'Connecting...');
    
    try {
        ws = new WebSocket(serverUrl);
        
        ws.onopen = () => {
            console.log('Connected');
            reconnectAttempts = 0;
            reconnectDelay = 1000;
            updateStatus('connected', 'Connected');
            startHeartbeat();
        };
        
        ws.onmessage = (event) => {
            const data = JSON.parse(event.data);
            handleMessage(data);
        };
        
        ws.onerror = (error) => {
            console.error('WebSocket error:', error);
        };
        
        ws.onclose = () => {
            console.log('Disconnected');
            stopHeartbeat();
            updateStatus('disconnected', 'Disconnected');
            
            messageInput.disabled = true;
            sendBtn.disabled = true;
            skipBtn.disabled = true;
            
            // Attempt to reconnect
            attemptReconnect();
        };
    } catch (error) {
        console.error('Connection error:', error);
        attemptReconnect();
    }
}

function attemptReconnect() {
    if (reconnectAttempts < maxReconnectAttempts) {
        reconnectAttempts++;
        updateStatus('reconnecting', `Reconnecting (${reconnectAttempts}/${maxReconnectAttempts})...`);
        
        setTimeout(() => {
            connect();
        }, reconnectDelay);
        
        // Exponential backoff
        reconnectDelay = Math.min(reconnectDelay * 2, 30000); // Max 30 seconds
    } else {
        updateStatus('disconnected', 'Connection failed. Refresh to retry.');
        addSystemMessage('❌ Could not reconnect. Please refresh the page.');
    }
}

function startHeartbeat() {
    heartbeatInterval = setInterval(() => {
        if (ws && ws.readyState === WebSocket.OPEN) {
            // Server sends pings, we respond with pongs
            // This is handled automatically in handleMessage
        }
    }, 30000); // 30 seconds
}

function stopHeartbeat() {
    if (heartbeatInterval) {
        clearInterval(heartbeatInterval);
        heartbeatInterval = null;
    }
}

// ==================== MESSAGE HANDLING ====================

function handleMessage(data) {
    if (data.type === 'ping') {
        // Respond to server ping with pong
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ type: 'pong' }));
        }
        return;
    }
    
    if (data.type === 'info') {
        addSystemMessage(data.message);
    }
    else if (data.type === 'session_id') {
        console.log('Session ID:', data.message);
    }
    else if (data.type === 'status') {
        if (data.message === 'Matched') {
            isMatched = true;
            updateStatus('matched', 'Chatting');
            addSystemMessage('🎉 Connected with a stranger!');
            messageInput.disabled = false;
            updateSendButton();
            skipBtn.disabled = false;
            messageInput.focus();
        }
        else if (data.message === 'Waiting') {
            isMatched = false;
            updateStatus('waiting', 'Searching...');
            messageInput.disabled = true;
            sendBtn.disabled = true;
            skipBtn.disabled = true;
            removeTypingIndicator();
        }
    }
    else if (data.type === 'chat') {
        addReceivedMessage(data.message);
        removeTypingIndicator();
    }
    else if (data.type === 'typing') {
        if (data.status) {
            showTypingIndicator();
        } else {
            removeTypingIndicator();
        }
    }
    else if (data.type === 'error') {
        addSystemMessage('❌ ' + data.message);
    }
}

// ==================== UI UPDATES ====================

function removeEmptyState() {
    if (emptyState) {
        emptyState.remove();
    }
}

function addSystemMessage(text) {
    removeEmptyState();
    const msg = document.createElement('div');
    msg.className = 'message system';
    msg.textContent = text;
    messagesContainer.appendChild(msg);
    scrollToBottom();
}

function addSentMessage(text) {
    removeEmptyState();
    const msg = document.createElement('div');
    msg.className = 'message sent';
    
    const time = new Date().toLocaleTimeString('en-US', {
        hour: 'numeric',
        minute: '2-digit'
    });
    
    msg.innerHTML = `
        <div>${escapeHtml(text)}</div>
        <div class="message-time">${time}</div>
    `;
    messagesContainer.appendChild(msg);
    scrollToBottom();
}

function addReceivedMessage(text) {
    removeEmptyState();
    removeTypingIndicator();
    
    const msg = document.createElement('div');
    msg.className = 'message received';
    
    const time = new Date().toLocaleTimeString('en-US', {
        hour: 'numeric',
        minute: '2-digit'
    });
    
    msg.innerHTML = `
        <div>${escapeHtml(text)}</div>
        <div class="message-time">${time}</div>
    `;
    messagesContainer.appendChild(msg);
    scrollToBottom();
}

function showTypingIndicator() {
    if (!typingIndicator) {
        removeEmptyState();
        typingIndicator = document.createElement('div');
        typingIndicator.className = 'typing-indicator';
        typingIndicator.innerHTML = '<span></span><span></span><span></span>';
        messagesContainer.appendChild(typingIndicator);
        scrollToBottom();
    }
}

function removeTypingIndicator() {
    if (typingIndicator) {
        typingIndicator.remove();
        typingIndicator = null;
    }
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

function scrollToBottom() {
    messagesContainer.scrollTop = messagesContainer.scrollHeight;
}

function updateStatus(className, text) {
    const status = document.getElementById('status');
    status.className = className;
    status.textContent = text;
}

// ==================== USER ACTIONS ====================

function sendMessage() {
    const message = messageInput.value.trim();
    
    if (message && isMatched && ws.readyState === WebSocket.OPEN) {
        if (message.length > 500) {
            addSystemMessage('❌ Message too long (max 500 characters)');
            return;
        }
        
        ws.send(JSON.stringify({
            type: 'msg',
            text: message
        }));
        
        addSentMessage(message);
        messageInput.value = '';
        updateCharCount();
        updateSendButton();
        
        // Stop typing indicator
        sendTypingStatus(false);
    }
}

function skipPartner() {
    if (isMatched && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({
            type: 'skip'
        }));
        
        isMatched = false;
        addSystemMessage('🔍 Looking for new chat partner...');
        messageInput.disabled = true;
        sendBtn.disabled = true;
        skipBtn.disabled = true;
        removeTypingIndicator();
    }
}

function sendTypingStatus(isTyping) {
    if (ws && ws.readyState === WebSocket.OPEN && isMatched) {
        ws.send(JSON.stringify({
            type: 'typing',
            status: isTyping
        }));
    }
}

function updateCharCount() {
    const count = messageInput.value.length;
    charCount.textContent = `${count}/500`;
    
    if (count > 500) {
        charCount.className = 'error';
        messageInput.value = messageInput.value.substring(0, 500);
    } else if (count > 450) {
        charCount.className = 'warning';
    } else {
        charCount.className = '';
    }
}

function updateSendButton() {
    sendBtn.disabled = !messageInput.value.trim() || !isMatched;
}

// ==================== EVENT LISTENERS ====================

messageInput.addEventListener('input', function() {
    updateCharCount();
    updateSendButton();
    
    // Auto-resize textarea
    this.style.height = 'auto';
    this.style.height = Math.min(this.scrollHeight, 100) + 'px';
    
    // Send typing indicator
    if (this.value.length > 0) {
        clearTimeout(typingTimeout);
        sendTypingStatus(true);
        
        typingTimeout = setTimeout(() => {
            sendTypingStatus(false);
        }, 3000);
    } else {
        sendTypingStatus(false);
    }
});

messageInput.addEventListener('keypress', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendMessage();
    }
});

// ==================== INITIALIZATION ====================

// Connect on load
connect();
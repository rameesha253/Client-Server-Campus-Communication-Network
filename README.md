# **Campus Communication Network – Client–Server System (C++ | TCP & UDP)**

## 📌 **Project Overview**

This project implements a fully functional **Client–Server communication system** designed for a university’s multi-campus network. Multiple remote campuses connect to a central server located in **Islamabad**, enabling real-time messaging, file sharing, and administrative broadcasts.
The entire system is built in **C++**, using a combination of **TCP (reliable communication)** and **UDP (lightweight heartbeat monitoring)**.

This system demonstrates key Computer Networks concepts such as socket programming, concurrency, message routing, protocol design, and distributed communication.

---

## ✨ **Key Features**

### 📍 **Central Server – Islamabad**

The server is the central communication hub responsible for:

* **Authenticating all campus clients**
* **Routing text messages** to target campuses
* **Handling file transfers** (receive → save → forward)
* **Tracking client online status** using UDP heartbeat packets
* Running a full **Admin Console** for monitoring and control:

  * View list of connected campuses
  * Check last heartbeat timestamps
  * Broadcast announcements to all campuses
  * List & open files received at Islamabad
  * Gracefully shut down the server and all connections

### 🎓 **Campus Client – Remote Campuses**

Each campus runs its own client system that allows:

* **Secure authentication** using campus credentials
* Sending & receiving **messages** via server routing
* Sending & receiving **text-based files**
* Saving received files automatically
* Opening files directly inside the console
* Receiving **server-wide admin broadcast announcements**
* Sending **UDP heartbeat packets every 5 seconds** to stay marked “online”

---

## 📡 **Communication Protocol**

A **custom-designed, text-based protocol** ensures clear, structured, and readable communication between nodes.

### 🔑 **Authentication**

```
Campus:<Name>;Pass:<Password>
```

### 💬 **Messaging**

```
SEND|TargetCampus|MessageText
```

### 📁 **File Transfer**

```
FILE|TargetCampus|FileName|Content
```

### ❤️ **Heartbeat (UDP)**

Sent every 5 seconds:

```
Campus:<Name>;HB:online
```

### 📢 **Admin Broadcast**

Plain text message sent to all clients:

```
<Broadcast Message>
```

---

## 🧬 **System Flow Summary**

### 1️⃣ **Client Authentication**

1. Client connects to server (TCP)
2. Sends authentication packet
3. Server validates credentials
4. Accepts or rejects connection
5. On success → client appears in the server’s active campus list

### 2️⃣ **Messaging System**

* Client sends message to server (TCP)
* Server identifies the target campus
* Forwards message to the intended connected client
* Target prints the message on their console

### 3️⃣ **File Transfer Process**

* Client reads text file → sends it in protocol format
* Server:

  * Saves the file locally (for Islamabad)
  * Or forwards it to the target campus
* Receiving campus:

  * Stores file locally
  * Displays a confirmation message

### 4️⃣ **Heartbeat Monitoring (UDP)**

* Every 5 seconds, clients send UDP packets
* Server updates:

  * Online/offline status
  * Last heartbeat timestamp
* Used for real-time campus availability tracking

### 5️⃣ **Admin Tools**

The admin console inside the server provides:

* **Client list** with online/offline status
* **Announcements** to all connected campuses
* **File viewer** for received files
* **Server shutdown** command to terminate all connections safely

---

## ⚙️ **Compilation Instructions (Ubuntu/Linux)**

### **Compile Server**

```
g++ server_with_admin.cpp -o server -lpthread
```

### **Compile Client**

```
g++ client.cpp -o client -lpthread
```

### **Run Server**

```
./server
```

### **Run Multiple Clients (Each in separate terminal)**

```
./client
```

---

## 🧠 **Core Concepts Demonstrated**

This project covers several major topics in Computer Networks:

* TCP and UDP socket programming
* Custom communication protocol design
* Multi-threaded server architecture
* Message forwarding logic
* File transfer over TCP
* Heartbeat monitoring using UDP
* Connection management & concurrency
* Admin-level server controls

---

## 🎯 **Conclusion**

This project combines theoretical networking principles with hands-on implementation to create a **real-time distributed communication system**. By integrating both TCP and UDP, developing a custom protocol, and building an admin interface, the system reflects how real-world campus networks and distributed applications are designed.
---


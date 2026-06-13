# Pet Feeder IoT

An automatic pet feeding system built on ESP32, combining a load cell for food weight measurement, a real-time clock for scheduling, and cloud sync for remote monitoring through a mobile app.

## Goal

Most commercial pet feeders only support time-based feeding. This project aims to go further by:

- Measuring actual food weight dispensed and remaining in the bowl, not just opening a gate on a timer
- Avoiding overfeeding by skipping dispensing if the bowl still has enough food
- Detecting and recovering from mechanical jams during dispensing
- Tracking daily eating patterns (number of meals, total amount eaten) to help owners notice changes in their pet's appetite early

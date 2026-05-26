<img width="2966" height="68" alt="image" src="https://github.com/user-attachments/assets/01d6a088-de28-4b8b-bdbe-efb17c75d329" /># DWM Memory Spike Monitor

Small Windows utility that watches `dwm.exe` for large memory jumps and writes timestamped events to `dwm_spikes.csv`.

I created this tool to diagnose the persistent issues I had with the Dynamic Window Manager process, it helped get to the root of the problem by diagnosing which exact process was the biggest culprit.


<img width="1422" height="710" alt="image" src="https://github.com/user-attachments/assets/c082e40e-cbda-443a-9b5b-fd2101a36d92" />




# In the video below you'll see the monitoring tool as I open a few instances of chrome and then closing them all at once (triggering a temporary spike) 

https://github.com/user-attachments/assets/f13cca1e-c57c-47eb-b734-3442f5ac658b



In order to run this you either need to open a new Console project in Visual studio and then copy main.cpp or try to run through the slnx file.


* CSV results *

<img width="2966" height="68" alt="image" src="https://github.com/user-attachments/assets/485da9fb-042c-4740-ba48-2c95a81eb177" />

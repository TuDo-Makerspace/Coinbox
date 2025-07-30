import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv("adc_readings.csv")
plt.figure()
plt.plot(df["time_s"], df["value"], "-", linewidth=1)
plt.ylim(0, 1023)
plt.xlabel("Time (s)")
plt.ylabel("ADC value")
plt.title("Photo-diode readings")
plt.tight_layout()
plt.show()

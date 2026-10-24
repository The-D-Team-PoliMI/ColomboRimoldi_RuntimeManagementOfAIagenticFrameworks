import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd

data_df = pd.read_csv('Output/result.csv')
config = {"chart_type":"bar","title":"Total Revenue for Promo vs Non-Promo Sales in 2023","x_axis":"sale_type","y_axis":"total_revenue"}

# --- Generated Code ---
import matplotlib.pyplot as plt
import pandas as pd

x_data = data_df[config['x_axis']]
y_data = data_df[config['y_axis']]

plt.figure(figsize=(10, 6))
plt.bar(x_data, y_data)
plt.xlabel(config['x_axis'])
plt.ylabel(config['y_axis'])
plt.title(config['title'])
plt.xticks(rotation=45, ha='right')
plt.tight_layout()

# Salva il grafico generato
plt.savefig('Output/chart.png')
plt.close('all')

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd

data_df = pd.read_csv('Output/result.csv')
config = {"chart_type":"bar","group_by":"year","title":"Average Monthly Revenue per Region: 2022 vs 2023","x_axis":"region","y_axis":"avg_monthly_revenue"}

# --- Generated Code ---
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

group_col = config['group_by']
x_col = config['x_axis']
y_col = config['y_axis']

groups = sorted(data_df[group_col].unique())
x_labels = sorted(data_df[x_col].unique())
x = np.arange(len(x_labels))
n_series = len(groups)
bar_width = 0.8 / n_series

plt.figure(figsize=(12, 6))
for i, group_val in enumerate(groups):
    df_group = data_df[data_df[group_col] == group_val].set_index(x_col)
    y_vals = [df_group.loc[xl, y_col] if xl in df_group.index else 0 for xl in x_labels]
    offset = (i - n_series / 2 + 0.5) * bar_width
    plt.bar(x + offset, y_vals, width=bar_width, label=str(group_val))

plt.xlabel(x_col)
plt.ylabel(y_col)
plt.title(config['title'])
plt.xticks(x, x_labels, rotation=45, ha='right')
plt.legend(title=group_col)
plt.grid(True, axis='y', alpha=0.3)
plt.tight_layout()

# Salva il grafico generato
plt.savefig('Output/chart.png')
plt.close('all')

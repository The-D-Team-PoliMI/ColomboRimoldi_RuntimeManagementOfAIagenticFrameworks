import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd

data_df = pd.read_csv('Output/result.csv')
config = {"chart_type":"bar"}

# --- Generated Code ---
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

x_axis = config['x_axis']
title = config['title']

plt.figure(figsize=(12, 6))

if 'y_axes' in config:
    y_axes = config['y_axes']
    n_series = len(y_axes)
    bar_width = 0.8 / n_series
    x_labels = data_df[x_axis].astype(str)
    x = np.arange(len(x_labels))
    
    for i, col in enumerate(y_axes):
        offset = (i - n_series / 2 + 0.5) * bar_width
        plt.bar(x + offset, data_df[col], width=bar_width, label=col)
        
    plt.xticks(x, x_labels, rotation=45, ha='right')
    plt.legend()
    plt.ylabel('Value')

elif 'group_by' in config:
    group_col = config['group_by']
    y_col = config['y_axis']
    groups = sorted(data_df[group_col].unique())
    x_labels = sorted(data_df[x_axis].unique())
    x = np.arange(len(x_labels))
    n_series = len(groups)
    bar_width = 0.8 / n_series
    
    for i, group_val in enumerate(groups):
        df_group = data_df[data_df[group_col] == group_val].set_index(x_axis)
        y_vals = [df_group.loc[xl, y_col] if xl in df_group.index else 0 for xl in x_labels]
        offset = (i - n_series / 2 + 0.5) * bar_width
        plt.bar(x + offset, y_vals, width=bar_width, label=str(group_val))
        
    plt.xticks(x, x_labels, rotation=45, ha='right')
    plt.legend()
    plt.ylabel(y_col)

else:
    y_axis = config['y_axis']
    x_data = data_df[x_axis]
    y_data = data_df[y_axis]
    plt.bar(x_data, y_data)
    plt.xticks(rotation=45, ha='right')
    plt.ylabel(y_axis)

plt.xlabel(x_axis)
plt.title(title)
plt.grid(True, axis='y', alpha=0.3)
plt.tight_layout()

# Salva il grafico generato
plt.savefig('Output/chart.png')
plt.close('all')

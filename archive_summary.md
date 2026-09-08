# Cultures Gold 本地化内容存档汇总

生成时间: 2026-09-08

## 存档文件

| 文件 | 说明 | 格式 |
|------|------|------|
| archive_sal.tsv | SAL 界面文本 | source	line	ger	cn |
| archive_tab.tsv | TAB 菜单文本 | source	line	ger	cn |
| archive_cif.tsv | CIF 任务文本 | source	line	raw	text	cn |
| archive_fhll.tsv | FHLL 简报文本 | source	line	raw	text	cn |

## 统计

| 类别 | 总条目 | 已译 | 未译 |
|------|--------|------|------|
| SAL 界面 | 977 | 937 | 40 |
| TAB 菜单 | 166 | 166 | 0 |
| CIF 任务 | 1252 | 386 | 866 |
| FHLL 简报 | 1945 | 2 | 1943 |
| **合计** | **4340** | **1491** | **2849** |

## 说明

- SAL: 游戏内界面文本（建筑名、物品名、菜单项等），走 sub_48DEA0 hook，词典 key = 裸文本行
- TAB: 主菜单/设置界面文本，走 sub_48DEA0 hook，词典 key = 裸文本行
- CIF: 任务/地图文本（部落名、人名、任务目标），走 sub_48DEA0 hook，词典 key = \x02 + sn/s 行
- FHLL: 简报正文（任务过场、教程说明），不走 sub_48DEA0 hook，需通过渲染层 hook 翻译

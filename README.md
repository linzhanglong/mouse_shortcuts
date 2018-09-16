鼠标的快捷键，就是点击几下鼠标就可以自动执行一些软件:

我们的鼠标有左键，中键，右键。那么我是怎么设计的，中键表示开始和结束，在开始和结束之间统计左键的次数，根据这个次数来运行我们的程序。并且这里加上一个超时时间，就是按键的时间间隔是10秒内有效，超过时秒就作废。


快捷键的配置文件：
$ cat  /etc/mouse_shortcuts.conf 

0:/usr/bin/xed /etc/mouse_shortcuts.conf

1:/opt/teamviewer/tv_bin/TeamViewer

2:/opt/google/chrome/chrome --no-sandbox --url https://wx.qq.com/

3:/opt/google/chrome/chrome --no-sandbox --url https://y.qq.com/

4:/opt/google/chrome/chrome --no-sandbox https://web.kd77.cn/

5:poweroff


字段格式说明：
左键次数：需要执行的命令





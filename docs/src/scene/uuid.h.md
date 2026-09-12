# 文件名：uuid.h

## 文件作用

这个文件是干什么的：它定义了一个叫 UUID 的类型，用来给场景里的物体发一个独一无二的编号，编号是"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"这种带横线的字符串格式。里面声明了怎么新建 UUID、怎么判断一个 UUID 对不对、怎么比较两个 UUID 是否一样，还有生成新 UUID 和拿一个全零的"空" UUID 的方法。它还声明了把 UUID 当成普通字符串去排序和查找的方法。

---
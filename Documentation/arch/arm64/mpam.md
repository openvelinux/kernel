MPAM 用户手册
=========

# 1 MPAM 简介

MPAM（Memory system component Partioning and Monitoring）是Arm Architecture v8.4的拓展特性。用于解决服务器系统中，混合部署不同类型业务时，由于共享资源的竞争（L3/L2 Cache，MATA），而带来的关键应用性能下降或者系统整体性能下降问题。

MPAM的应用可针对不同业务，将同时作用于硬件访存路径上产生的竞争和冲突进行隔离控制，从而帮助提升服务器利用率，降低服务部署成本。

本手册只适用于OLK-6.6软件版本。

# 2 内核编译选项

配置 CONFIG_ARM64_MPAM=y 后，即使能MPAM完整功能。

# 3 内核启动参数

启动流程默认关闭MPAM，启动MPAM初始化需要**cmdline**添加**arm64.mpam**参数配置后重启机器。

# 3 接口总览

MPAM功能通过resctrl文件系统呈现，挂载点位于 */sys/fs/resctrl* 。系统启动后，需要手动挂载resctrl文件系统。

## 3.1 系统挂载参数

resctrl可以通过添加挂载参数支持多种挂载方式，具体指令如下：

~~~
# mount -t resctrl resctrl [-o cdp[,cdpl2][,debug][,l2]] /sys/fs/resctrl
~~~

挂载参数包括：

* cdp: 针对L3缓存，根据访问指令和访问数据分别配置。
* cdpl2: 针对L2缓存，根据访问指令和访问数据分别配置。
* debug: 使能调试接口访问。
* l2: 使能L2缓存配置和监控功能，默认关闭MPAM L2功能。

## 3.2 resctrl 系统目录介绍

### 3.2.1 Info 目录

Info 目录包含有关已启用资源的信息，每个资源都有其自己的子目录，子目录的名称反映了资源的名称。

每个子目录包含以下与分配相关的文件：

缓存资源（L3/L2）子目录包含以下与分配相关的文件：

**num_closids**: 适用于该资源的有效CLOSID（Class of Service ID）数量。内核会以所有已启用资源中最小的CLOSID数量作为限制。

**cbm_mask**: 适用于该资源的有效位掩码（bitmask）。

**min_cbm_bits**: 写入掩码时必须设置的连续位的最小数量。

**shareable_bits**: 与其他执行实体共享资源的位掩码。用户在设置独占缓存分区时可以使用此字段。

**bit_usage**: 标注了资源所有实例的使用情况的容量位掩码。说明如下：

  * *0*: 对应区域未使用。当系统的资源已被分配，且在“bit_usage”中发现“0”时，这表明资源被浪费了。

  * *H*: 对应区域仅由硬件使用，但可供软件使用。如果资源的“shareable_bits”中有位被设置，但这些位并未全部出现在资源组的分配方案中，则“shareable_bits”中出现但未分配给资源组的位将被标记为“H”。

  * *X*: 对应区域可供共享，并被硬件和软件使用。这些位同时出现在“shareable_bits”和资源组的分配中。

  * *S*: 对应区域由软件使用，并可供共享。

  * *E*: 对应区域被一个资源组独占使用，不允许共享。

**sparse_masks**: 指示是否支持CBM（Capacity Bit Mask）中的非连续1值。

  * *0*: 仅支持CBM中的连续1值。

  * *1*: 支持CBM中的非连续1值。

MB（Memory bandwidth，内存带宽）子目录包含以下与分配相关的文件：

**min_bandwidth**: 用户可以请求的最小内存带宽百分比。

**bandwidth_gran**: 内存带宽百分比分配的粒度。分配的带宽百分比会四舍五入到硬件上可用的下一个控制步长。可用的带宽控制步长为：
~~~
min_bandwidth + N * bandwidth_gran
~~~

**delay_linear**: 指示延迟刻度是线性还是非线性的。该字段仅用于信息参考。

如果支持监控功能，则会存在一个名为 L3_MON 和 MB_MON 的目录，其中包含以下文件：

**num_rmids**: 可用的RMID（Resource Monitoring ID）数量。这是可以创建的“CTRL_MON”+“MON”组的最大数量。

**mon_features**: 如果为该资源启用了监控功能，则列出监控事件。例如：
~~~
# grep . /sys/fs/resctrl/info/*_MON/mon_features
/sys/fs/resctrl/info/L3_MON/mon_features:llc_occupancy
/sys/fs/resctrl/info/MB_MON/mon_features:mbm_total_bytes
~~~

**max_threshold_occupancy**: 读/写文件，提供一个最大值（以字节为单位），低于此设定值下，之前使用过的LLC_occupancy计数器可以被考虑重新分配使用。

> 请注意，一旦释放了RMID（资源监控ID），它可能不会立即可用，因为RMID仍然与之前使用该RMID的缓存行相关联。因此，这些RMID会被放入一个“待定”列表中，并在缓存占用量降低后再次检查。如果系统中存在大量处于“待定”状态的RMID，但它们尚未准备好被使用，用户在执行mkdir操作时可能会看到-EBUSY错误。

> max_threshold_occupancy是一个用户可配置的值，用于确定在什么占用量下可以释放一个RMID。

最后，在 Info 目录的顶层有一个名为 **last_cmd_status** 的文件。每次通过文件系统发出“命令”（例如创建新目录或写入任何控制文件）时，该文件都会被重置。如果命令成功，文件内容将显示为“ok”。如果命令失败，它将提供比文件操作错误返回更详细的信息。例如：
~~~
# echo "MB:1=110" > schemata
-bash: echo: write error: Invalid argument
# cat /sys/fs/resctrl/info/last_cmd_status
MB value 110 out of range [0,100]
~~~

### 3.2.2 资源分配和监控

资源组在resctrl文件系统中以目录的形式表示。默认组是根目录，刚挂载后，它拥有系统中的所有任务和CPU，并且可以充分利用所有资源。

在支持RDT（资源分配技术）控制功能的系统中，可以在根目录下创建额外的目录，这些目录指定了每种资源的不同数量（参见下面的“schemata”）。根目录和这些额外的顶级目录在下文中被称为“CTRL_MON”组。

在支持RDT监控的系统中，根目录和其他顶级目录中包含一个名为“mon_groups”的目录，在其中可以创建额外的目录来监控其父“CTRL_MON”组中任务的子集。这些在本文档的其余部分被称为“MON”组。

删除一个目录会将其所代表的组拥有的所有任务和CPU移动到其父目录。删除一个创建的“CTRL_MON”组将自动删除其下所有的“MON”组。

支持将“MON”组目录移动到一个新的父“CTRL_MON”组，以便在不影响其监控数据或分配的任务的情况下更改“MON”组的资源分配。此操作不适用于监控CPU的“MON”组。目前，除了简单地重命名“CTRL_MON”或“MON”组之外，不支持其他任何移动操作。

所有组包含以下文件：

**tasks**: 读取此文件将显示属于该组的所有任务列表。将任务ID写入该文件会将任务添加到该组中。可以通过用逗号分隔任务ID来添加多个任务。任务将按顺序分配。在尝试分配任务时遇到的任何单个失败都会导致操作中止，而在失败之前已添加到组中的任务将保留在组中。失败信息将记录到/sys/fs/resctrl/info/last_cmd_status。

如果该组是一个“CTRL_MON”组，则任务将从之前拥有该任务的“CTRL_MON”组中移除，同时也会从任何拥有该任务的“MON”组中移除。如果该组是一个“MON”组，则任务必须已经属于该组的“CTRL_MON”父组。任务将从任何之前的“MON”组中移除。

**cpus**: 读取此文件将显示该组拥有的逻辑CPU的位掩码。将掩码写入该文件将向该组添加或移除CPU。与“tasks”文件类似，维护了一个层级结构，其中“MON”组只能包含其父“CTRL_MON”组拥有的CPU。

**cpus_list**: 与“cpus”类似，但使用CPU范围而不是位掩码。

当启用控制功能时，所有“CTRL_MON”组还将包含以下文件：

**schemata**: 列出该组可用的所有资源。每种资源都有自己的行和格式——详细信息请参见下文。

**size**: 类似于“schemata”文件的显示，但显示的是每种资源分配的字节大小，而不是表示分配的位。

**mode**: 资源组的“mode”决定了其分配的共享方式。“shareable”资源组允许共享其分配，而“exclusive”资源组则不允许。

**ctrl_hw_id**: 仅在启用调试选项时可用。硬件用于控制组的标识符。在arm64架构上，即是 PARTID。

当启用监控功能时，所有“MON”组还将包含以下文件：

**mon_data**: 包含一组按L3域和MB域事件组织的文件。例如，在具有两个L3域的系统中，将存在子目录“mon_L3_00”和“mon_L3_01”。

每个子目录中都有一个文件对应每个事件（例如“llc_occupancy”、“mbm_total_bytes”）。在“MON”组中，这些文件提供了组中所有任务的当前事件值。在“CTRL_MON”组中，这些文件提供了“CTRL_MON”组中所有任务以及所有“MON”组中任务的总和。有关使用方法的更多详细信息，请参见示例部分。

**mon_hw_id**: 仅在启用调试选项时可用。硬件用于监控组的标识符。

以下为resctrl文件系统目录树：

~~~
/sys/fs/resctrl(根分组)
 ├── cpus                      # bitmask方式显示根分组关联的vcpu
 ├── cpus_list                 # cpu list方式显示根分组关联的vcpu
 ├── ctrl_hw_id                # 硬件用于控制组的标识符
 ├── info                      # 用于显示属性信息及错误提示信息
 │   ├── L3
 │   │   ├── bit_usage         # 标注了资源所有实例的使用情况的容量位掩码
 │   │   ├── cbm_mask          # 系统所支持的最大cache way bitmask，一个bit代表一个cache way
 │   │   ├── min_cbm_bits      # 使用schemata所能配置的最小cache way bitmask
 │   │   ├── num_closids       # L3能够提供创建控制组的最大数量
 │   │   ├── shareable_bits    # 当前所有cbm_mask全部shareable，支持后续扩展
 │   │   └── sparse_masks      # 指示是否支持CBM（Capacity Bit Mask）中的非连续1值
 │   ├── L3_MON
 │   │   ├── max_threshold_occupancy # 低于此设定值下，之前使用过的LLC_occupancy计数器可以被考虑重新分配使用
 │   │   ├── mon_features      # 列出监控事件
 │   │   └── num_rmids         # 可创建控制组和监控组的总数
 │   ├── last_cmd_status       # 操作错误提示
 │   ├── MB
 │   │   ├── bandwidth_gran    # 带宽百分比配置粒度
 │   │   ├── delay_linear      # 指示延迟刻度是线性还是非线性的
 │   │   ├── min_bandwidth     # 最小带宽配置百分比
 │   │   └── num_closids       # 同L3 num_closid
 │   └── MB_MON
 │       ├── mon_features      # 同L3 mon_features
 │       └── num_rmids         # 同L3 num_rmids
 ├── mode                      # 资源组的 mode 决定了其分配的共享方式
 ├── mon_data
 │   ├── mon_L3_01             # 标号代表L3 cache id
 │   │   └── llc_occupancy     # 表示当前分组所关联的pid/vcpu在该区域上实际占用L3 Cache大小，下同
 │   ├── mon_L3_122
 │   │   └── llc_occupancy
 │   ├── mon_MB_00             # 标号代表numa id
 │   │   └── mbm_total_bytes   # 表示当前分组所关联的pid/vcpu在该区域上内存带宽流量大小，下同
 │   └── mon_MB_01
 │       └── mbm_total_bytes
 ├── mon_groups                # 创建监控组目录
 ├── mon_hw_id                 # 硬件用于监控组的标识符
 ├── schemata                  # 资源使用配置接口
 ├── size                      # 显示的是每种资源分配的字节大小
 └── tasks                     # 显示与根组关联的pid
~~~

### 3.2.3 控制组配置接口 Schemata 文件

**schemata**文件中的每一行描述一种资源。每行以资源的名称开头，后面跟着该资源在系统中每个实例上要应用的具体值。

#### Cache IDs

在当前一代的系统中，每个插槽（socket）有一个L3缓存，而L2缓存通常仅由一个核心上的超线程共享，但这并不是架构上的强制要求。我们可能会在一个插槽上有多个独立的L3缓存，或者多个核心共享一个L2缓存。因此，我们不使用“插槽”或“核心”来定义共享资源的逻辑CPU集合，而是使用“Cache ID”（缓存ID）。

在给定的缓存级别上，这将在整个系统中是一个唯一的数字（但不能保证是一个连续的序列，可能会有间隔）。要查找每个逻辑CPU的ID，请查看 /sys/devices/system/cpu/cpu*/cache/index*/id。

#### Cache Bit Masks（CBM，缓存位掩码）

对于缓存资源，我们使用位掩码来描述可用于分配的缓存部分。掩码的最大值由每种CPU型号定义（并且可能因不同的缓存级别而异）。该值在resctrl文件系统的“info”目录中提供，位于info/{resource}/cbm_mask。

#### L3 缓存配置

当未启用CDP（代码/数据缓存划分）时，L3 schemata的格式为：

~~~
L3:<cache_id0>=<cbm>;<cache_id1>=<cbm>;...
~~~

当启用CDP时，L3控制被拆分为两个独立的资源，因此您可以分别为代码和数据指定独立的掩码，如下所示：

~~~
L3DATA:<cache_id0>=<cbm>;<cache_id1>=<cbm>;...
L3CODE:<cache_id0>=<cbm>;<cache_id1>=<cbm>;...
~~~

读取schemata文件将显示所有域上所有资源的状态。在写入时，您只需要指定您希望更改的值。

例如使用默认方式挂载，设置L3缓存位掩码只有4位：

~~~
# mount -t resctrl resctrl /sys/fs/resctrl/
# cat /sys/fs/resctrl/schemata
L3:1=fffffff;122=fffffff

# echo "L3:122=3c0;" > /sys/fs/resctrl/schemata
# cat /sys/fs/resctrl/schemata
L3:1=fffffff;122=00003c0
~~~

使用开启CDP方式挂载，设置L3 data缓存位掩码只有4位：

~~~
# mount -t resctrl resctrl /sys/fs/resctrl/ -o cdp
# cat /sys/fs/resctrl/schemata
L3DATA:1=fffffff;122=fffffff
L3CODE:1=fffffff;122=fffffff

# echo "L3DATA:122=3c0;" > schemata
# cat /sys/fs/resctrl/schemata
L3DATA:1=fffffff;122=00003c0
L3CODE:1=fffffff;122=fffffff
~~~

#### L2 缓存配置

L2 缓存配置功能默认关闭，需要通过显式添加 l2 挂载参数，才会使能L2缓存配置功能。使能L2功能以后，系统关闭 cpuidle powerdown 功能和cpu下线功能。

L2 schemata的格式是：

~~~
L2:<cache_id0>=<cbm>;<cache_id1>=<cbm>;...
~~~

使用“cdpl2”挂载选项可以在L2上支持CDP：

~~~
L2DATA:<cache_id0>=<cbm>;<cache_id1>=<cbm>;...
L2CODE:<cache_id0>=<cbm>;<cache_id1>=<cbm>;...
~~~

L2 缓存配置示例，设置L2缓存位掩码只有4位：

~~~
# mount -t resctrl resctrl /sys/fs/resctrl/ -o l2
# cat schemata
L2:4=000ff;8=000ff

# echo "L2:4=f;" > schemata
# cat schemata
L2:4=0000f;8=000ff
~~~

使用“cdpl2”挂载选项：

~~~
# mount -t resctrl resctrl /sys/fs/resctrl/ -o l2cdp,l2
# cat schemata
L2DATA:4=000ff;8=000ff
L2CODE:4=000ff;8=000ff

# echo "L2DATA:4=f;" > schemata
# cat schemata
L2DATA:4=0000f;8=000ff                      # 控制组 L2 DATA 只能使用L2 cache4的4个cache way
L2CODE:4=000ff;8=000ff
~~~

#### MB 内存带宽分配

对于内存带宽资源，默认情况下，用户通过指定总内存带宽的百分比来控制该资源。

每种CPU型号的最小带宽百分比值是预定义的，可以通过info/MB/min_bandwidth查询。分配的带宽粒度也取决于CPU型号，可以在info/MB/bandwidth_gran中查询。可用的带宽控制步长为：min_bw + N * bw_gran。中间值会被四舍五入到硬件上可用的下一个控制步长。

MB schemata的格式是：

~~~
MB:<cache_id0>=bandwidth0;<cache_id1>=bandwidth1;...
~~~

MB 带宽配置示例：

~~~
# cat /sys/fs/resctrl/schemata
MB:0=0000100;1=0000100

# echo "MB:0=50" > /sys/fs/resctrl/schemata
# cat /sys/fs/resctrl/schemata
MB:0=0000050;1=0000100                      # 降低控制组 MB 内存带宽使用上限为50%
~~~

### 3.2.4 资源分配规则

当一个任务正在运行时，以下规则定义了它可用的资源：
1. 如果任务属于一个非默认组，则使用该组的分配方案（schemata）。
2. 否则，如果任务属于默认组，但运行在一个被分配给某个特定组的CPU上，则使用该CPU所属组的分配方案。
3. 否则，使用默认组的分配方案。

### 3.2.5 监控组配置方法

读取控制组和监控组的监控数据，可通过 mon_data 目录接口读取监控数据：

~~~
# grep . mon_data/*/*
mon_data/mon_L3_01/llc_occupancy:73276416
mon_data/mon_L3_122/llc_occupancy:11875328
mon_data/mon_MB_00/mbm_total_bytes:32806
mon_data/mon_MB_01/mbm_total_bytes:31700
~~~

其中，mon_data读取监控数据文件分别：
  * llc_occupancy 代表 L3缓存当前占用量，单位 Byte
  * mbm_total_bytes 代表内存带宽瞬时流量，单位 MB/s

支持在控制组下创建子监控组，监控父控制组监控对象的子集：

~~~
# cd /sys/fs/resctrl/p1
# cd mon_groups/ && mkdir m1                # 监控组只能监控，m1分组资源配置跟随p1分组
# echo '0-1' > cpus_list
# grep . mon_data/mon_*/*
mon_data/mon_L3_01/llc_occupancy:18432
mon_data/mon_L3_122/llc_occupancy:1024
mon_data/mon_MB_00/mbm_total_bytes:0
mon_data/mon_MB_01/mbm_total_bytes:0
~~~

控制组监控的是控制组本身及所有子监控组的监控值之和。

## 3.3 QoS 增强特性

### 3.3.1 PRI 优先级设置

对共享资源优先级进行配置，包括 L3PRI 和 MBPRI：

~~~
# cat schemata
 MBPRI:0=0000007;1=0000007
 L3PRI:1=0000003;122=0000003

# echo "MBPRI:0=0000003" > schemata
# cat schemata
 MBPRI:0=0000003;1=0000007                  # 降低控制组 MB numa0的优先级
 L3PRI:1=0000003;122=0000003

# echo "MBPRI:0=0000003" > schemata

~~~

优先级设置数字越大，即优先级越高，反之，数字越小，优先级越低。

> MBPRI 默认值为 3，MBPRI合法值范围 [0,7]。L3PRI 默认值为 0，L3PRI合法值范围 [0,3]。

### 3.3.2 MIN 限低值设置

共享资源实际使用占比低于设置值，会自动提高对该资源使用优先级，包括 L3MIN 和 MBMIN：

~~~
# cat schemata
 MBMIN:0=00100
 L3MIN:1=00100;5=00100

# echo "MBMIN:0=00050" > schemata
# cat schemata
 MBMIN:0=00050
 L3MIN:1=00100;5=00100

# echo "L3MIN:1=00050" > schemata
# cat schemata
 MBMIN:0=00050
 L3MIN:1=00050;5=00100
~~~

L3MIN 和 MBMIN 接口接受的输入参数为百分比，即设置值与总资源（内存带宽/缓存占用量）的占比。

> MBMIN 和 L3MIN 默认值为 0，合法值范围都为 [0,100]。

### 3.3.3 HDL 强制隔离设置

当MBHDL=1，限制MB共享资源使用量不能超出MB设置值，若MBHDL=0，则允许空闲情况下，MB共享资源使用量超过MB设置值：

~~~
# cat schemata
 MBHDL:0=0000001;1=0000001

# echo "MBHDL:0=0000000" > schemata
# cat schemata
 MBHDL:0=0000000;1=0000001                  # 关闭MB在numa0上的强制限制功能
~~~

> MBHDL 默认值为 1，合法值范围 [0,1]。

### 3.3.4 MAX 资源上限设置

设置允许分配的缓存容量的最大百分比，包括 L3MAX 接口：

~~~
# cat schemata
 L3MAX:1=00100;5=00100

# echo "L3MAX:1=00050" > schemata
# cat schemata
 L3MAX:1=00050;5=00100                      # 降低L3分配的缓存最大容量百分比
~~~

> MB 和 L3MAX 默认值为 100，MB合法值范围 [1,100]，L3MAX合法值范围 [0,100]。

## 3.4 外设 IO 流量管控

### 3.4.1 控制组绑定外设

MPAM 提供通过绑定 iommu_group ID，对设备IO流量进行带宽限制和监控。

譬如，控制网卡设备 eno2 ，首先查找该设备 PCI_SLOT 信息：

~~~
# cat /sys/class/net/eno2/device/uevent | grep PCI_SLOT
PCI_SLOT_NAME=0000:35:00.1
~~~

或者通过 ethtool 工具查看 bus-info 信息：

~~~
# ethtool -i eno2 | grep bus-info
bus-info: 0000:35:00.1
~~~

按照设备总线信息，查找到该设备所属的 iommu_group ：

~~~
# find /sys/kernel/iommu_groups/ -name "0000:35:00.1"
/sys/kernel/iommu_groups/17/devices/0000:35:00.1
~~~

或者通过 lspci 工具查看 iommu_group 信息：

~~~
# lspci -vvv -s 0000:35:00.1 | grep "IOMMU group"
	IOMMU group: 17
~~~

将查询到的group id 17，通过 tasks 接口绑定到指定的控制组下：

~~~
# cd /sys/fs/resctrl/p1/
# echo "iommu_group:17" > tasks
# cat tasks
iommu_group:17                              # 此时iommu_group 17 已被绑定到控制组p1
~~~

### 3.4.2 查看外设带宽流量

控制组绑定外设所属的iommu组后，可以查看设备流量带宽：

~~~
# grep . mon_data/mon_MB_0*/*
mon_data/mon_MB_00/mbm_total_bytes:0
mon_data/mon_MB_01/mbm_total_bytes:4230
~~~

### 3.4.3 配置外设带宽流量

通过MB配置接口，可以实现限制设备的流量带宽：

~~~
# echo "MB:1=0000001" > schemata
# cat schemata
   MB:0=0000100;1=0000000

# grep . mon_data/*/*
mon_data/mon_MB_00/mbm_total_bytes:0
mon_data/mon_MB_01/mbm_total_bytes:1208
~~~

# 4 控制组和监控组配置使用示例

/sys/fs/resctrl 默认为根分组，根分组可以创建若干个控制组，一个控制组既可以关联一组pid/tid，也可以关联一组cpu集合。

创建一个新的控制组，关联 pid/tid：

~~~
# cd /sys/fs/resctrl/ && mkdir p1
# cd p1 && echo $$ > tasks                  # 关联当前shell进程pid到p1组
# cat tasks                                 # 可查看成功关联的pid
29190
29607
~~~

也可以选择关联 cpu：

~~~
# cd p1 && echo '0-1' > cpus_list
# cat cpus_list                             # 可查看关联的cpu
0-1
~~~

查看可创建的控制组和监控组数量：

~~~
# cat info/L3/num_closids                   # 可在info对应的资源的目录下查看closid数量，即可以创建控制组的数量
32
# cat info/MB/num_closids
32
# cat info/L3_MON/num_rmids                 # 可以创建控制组和监控组的总数量
128
# cat info/MB_MON/num_rmids
128
~~~

通过配置控制组可达到隔离 L3 Cache/Memory Bandwidth 效果，通过读取对应分组mon_data接口可以获取该组资源占用情况，比如对一个控制组限制L3 Cache使用：

~~~
# cat info/L3/cbm_mask                      # 查看info目录下对应资源的属性
fffffff
# cat info/L3/min_cbm_bits
1

# cd /sys/fs/resctrl/p1
# cat schemata
   MB:0=0000100;1=0000100
   L3:1=fffffff;122=fffffff

# echo 'L3:0=1' > schemata                  # 配置1条cache way给p1分组
# cat schemata
   MB:0=0000100;1=0000100                   # 若此时该组关联pid/cpu，那么该pid/cpu产生的访存请求只会分配到这条cache
   L3:1=0000001;122=fffffff
~~~

对控制组限制MB使用：

~~~
# cat info/MB/min_bandwidth                 # 和配置L3类似，也可以查看MB的相关信息
1
# cat info/MB/bandwidth_gran
1                                           # 可知，配置带宽最小百分比是1%，颗粒度是1%
# cat schemata
   MB:0=0000100;1=0000100
   L3:1=0000001;122=fffffff
# echo 'MB:0=1' > schemata
# cat schemata
   MB:0=0000001;1=0000100
   L3:1=0000001;122=fffffff
~~~

支持在控制组下创建子监控组：

~~~
# cd /sys/fs/resctrl/p1
# cd mon_groups/ && mkdir m1                # 监控组只能监控，m1分组资源配置跟随p1分组
# ls m1/
cpus  cpus_list  mon_data  tasks
# echo '0-1' > cpus_list
# cat cpus_list
0-1
# grep . mon_data/mon_*/*
mon_data/mon_L3_01/llc_occupancy:18432
mon_data/mon_L3_122/llc_occupancy:1024
mon_data/mon_MB_00/mbm_total_bytes:0
mon_data/mon_MB_01/mbm_total_bytes:0
~~~

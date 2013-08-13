
#include "ctp_platform_ops.h"

#include <plat/sys_config.h>
#include "gslx680_ts.h"

static void* __iomem gpio_addr = NULL;
static int gpio_int_hdle = 0;
static int gpio_wakeup_hdle = 0;
static int gpio_io_hdle = 0;

static user_gpio_set_t gpio_int_info[1];
#define CTP_IRQ_NO			(gpio_int_info[0].port_num)

#define GSLX680_NAME "gslx680"

#define GSL_DATA_REG		0x80
#define GSL_STATUS_REG		0xe0
#define GSL_PAGE_REG		0xf0

#define MAX_FINGERS 		10
#define MAX_CONTACTS 		10

#define CTP_IRQ_MODE			(NEGATIVE_EDGE)


#define DMA_TRANS_LEN		0x20


static int screen_max_x = 0;
static int screen_max_y = 0;
#define SCREEN_MAX_X			(screen_max_x)
#define SCREEN_MAX_Y			(screen_max_y)
#define PRESS_MAX    			255
static int revert_x_flag = 0;
static int revert_y_flag = 0;
static int exchange_x_y_flag = 0;
static int	int_cfg_addr[]={PIO_INT_CFG0_OFFSET,PIO_INT_CFG1_OFFSET,
			PIO_INT_CFG2_OFFSET, PIO_INT_CFG3_OFFSET};


static union{
	unsigned short dirty_addr_buf[2];
	const unsigned short normal_i2c[2];
} u_i2c_addr = {{0x00},};
static __u32 twi_id = 0;



struct gsl_ts_data {
	u8 x_index;
	u8 y_index;
	u8 z_index;
	u8 id_index;
	u8 touch_index;
	u8 data_reg;
	u8 status_reg;
	u8 data_size;
	u8 touch_bytes;
	u8 update_data;
	u8 touch_meta_data;
	u8 finger_size;
};

static struct gsl_ts_data devices[] = {
	{
		.x_index = 6,
		.y_index = 4,
		.z_index = 5,
		.id_index = 7,
		.data_reg = GSL_DATA_REG,
		.status_reg = GSL_STATUS_REG,
		.update_data = 0x4,
		.touch_bytes = 4,
		.touch_meta_data = 4,
		.finger_size = 70,
	},
};

struct gsl_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct work_struct work;
	struct workqueue_struct *wq;
	struct gsl_ts_data *dd;
	u8 *touch_data;
	u8 device_id;
	u8 prev_touches;
	bool is_suspended;
	bool int_pending;
	struct mutex sus_lock;
};


static int gsl_ts_write(struct i2c_client *client, u8 reg, u8 *pdata, int datalen)
{
	int ret = 0;
	u8 tmp_buf[128];
	unsigned int bytelen = 0;
	if (datalen > 125)
	{
		printk("%s too big datalen = %d!\n", __func__, datalen);
		return -1;
	}
	
	tmp_buf[0] = reg;
	bytelen++;
	
	if (datalen != 0 && pdata != NULL)
	{
		memcpy(&tmp_buf[bytelen], pdata, datalen);
		bytelen += datalen;
	}
	
	ret = i2c_master_send(client, tmp_buf, bytelen);
	return ret;
}

static int gsl_ts_read(struct i2c_client *client, u8 reg, u8 *pdata, unsigned int datalen)
{
	int ret = 0;

	if (datalen > 126)
	{
		printk("%s too big datalen = %d!\n", __func__, datalen);
		return -1;
	}

	ret = gsl_ts_write(client, reg, NULL, 0);
	if (ret < 0)
	{
		printk("%s set data address fail!\n", __func__);
		return ret;
	}
	
	return i2c_master_recv(client, pdata, datalen);
}


static u32 id_sign[MAX_CONTACTS+1] = {0};
static u8 id_state_flag[MAX_CONTACTS+1] = {0};
static u8 id_state_old_flag[MAX_CONTACTS+1] = {0};
static u16 x_old[MAX_CONTACTS+1] = {0};
static u16 y_old[MAX_CONTACTS+1] = {0};
static u16 x_new = 0;
static u16 y_new = 0;

static u8 gpio_init_status = 0;


static void record_point(u16 x, u16 y , u8 id)
{
	u16 x_err =0;
	u16 y_err =0;

	id_sign[id]=id_sign[id]+1;
	
	if(id_sign[id]==1){
		x_old[id]=x;
		y_old[id]=y;
	}

	x = (x_old[id] + x)/2;
	y = (y_old[id] + y)/2;
		
	if(x>x_old[id]){
		x_err=x -x_old[id];
	}
	else{
		x_err=x_old[id]-x;
	}

	if(y>y_old[id]){
		y_err=y -y_old[id];
	}
	else{
		y_err=y_old[id]-y;
	}

	if( (x_err > 6 && y_err > 2) || (x_err > 2 && y_err > 6) ){
		x_new = x;     x_old[id] = x;
		y_new = y;     y_old[id] = y;
	}
	else{
		if(x_err > 6){
			x_new = x;     x_old[id] = x;
		}
		else
			x_new = x_old[id];
		if(y_err> 6){
			y_new = y;     y_old[id] = y;
		}
		else
			y_new = y_old[id];
	}

	if(id_sign[id]==1){
		x_new= x_old[id];
		y_new= y_old[id];
	}
}



static inline u16 join_bytes(u8 a, u8 b)
{
	u16 ab = 0;
	ab = ab | a;
	ab = ab << 8 | b;
	return ab;
}


#ifdef HAVE_TOUCH_KEY
static void report_key(struct gsl_ts *ts, u16 x, u16 y)
{
	u16 i = 0;

	for(i = 0; i < MAX_KEY_NUM; i++) 
	{
		if((gsl_key_data[i].x_min < x) && (x < gsl_key_data[i].x_max)&&(gsl_key_data[i].y_min < y) && (y < gsl_key_data[i].y_max))
		{
			key = gsl_key_data[i].key;	
			input_report_key(ts->input, key, 1);
			input_sync(ts->input); 		
			key_state_flag = 1;
			break;
		}
	}
}
#endif

static void report_data(struct gsl_ts *ts, u16 x, u16 y, u8 pressure, u8 id)
{
	swap(x, y);

	//pr_info("#####id=%d,x=%d,y=%d######\n",id,x,y);

	if(x>=SCREEN_MAX_X||y>=SCREEN_MAX_Y)
	{
	#ifdef HAVE_TOUCH_KEY
		report_key(ts,x,y);
	#endif
		return;
	}

	if (exchange_x_y_flag)
		swap(x, y);

	if (revert_x_flag)
		x=SCREEN_MAX_X-x;

	if (revert_y_flag)
		y=SCREEN_MAX_Y-y;
	
#ifdef REPORT_DATA_ANDROID_4_0
	input_mt_slot(ts->input, id);		
	input_report_abs(ts->input, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, pressure);
	input_report_abs(ts->input, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input, ABS_MT_POSITION_Y, y);	
	input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR, 1);
#else
	input_report_abs(ts->input, ABS_X, x);
	input_report_abs(ts->input, ABS_Y, y);
	input_report_abs(ts->input, ABS_PRESSURE, pressure);
	input_report_key(ts->input, BTN_TOUCH, 1);

	input_report_abs(ts->input, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, pressure);
	input_report_abs(ts->input, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR, 1);
	input_mt_sync(ts->input);
#endif
}

static void process_gslX680_data(struct gsl_ts *ts)
{
	u8 id, touches;
	u16 x, y;
	int i = 0;

	touches = ts->touch_data[ts->dd->touch_index];
	for(i=1;i<=MAX_CONTACTS;i++)
	{
		if(touches == 0)
			id_sign[i] = 0;	
		id_state_flag[i] = 0;
	}
	
	if (touches == 0)
		input_report_key(ts->input, BTN_TOUCH, 0);
	else
		for(i= 0;i < (touches > MAX_FINGERS ? MAX_FINGERS : touches);i ++)
		{
			x = join_bytes( ( ts->touch_data[ts->dd->x_index  + 4 * i + 1] & 0xf),
					ts->touch_data[ts->dd->x_index + 4 * i]);
			y = join_bytes(ts->touch_data[ts->dd->y_index + 4 * i + 1],
					ts->touch_data[ts->dd->y_index + 4 * i ]);
			id = ts->touch_data[ts->dd->id_index + 4 * i] >> 4;

			if(1 <=id && id <= MAX_CONTACTS)
			{
				record_point(x, y , id);
				report_data(ts, x_new, y_new, 10, id);
				id_state_flag[id] = 1;
			}
		}
	for(i=1;i<=MAX_CONTACTS;i++)
	{	
		if( (0 == touches) || ((0 != id_state_old_flag[i]) && (0 == id_state_flag[i])) )
		{
		#ifdef REPORT_DATA_ANDROID_4_0
			input_mt_slot(ts->input, i);
			input_report_abs(ts->input, ABS_MT_TRACKING_ID, -1);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		#endif
			id_sign[i]=0;
		}
		id_state_old_flag[i] = id_state_flag[i];
	}
#ifndef REPORT_DATA_ANDROID_4_0
	if(0 == touches)
	{	
		input_mt_sync(ts->input);
	#ifdef HAVE_TOUCH_KEY
		if(key_state_flag)
		{
        		input_report_key(ts->input, key, 0);
			input_sync(ts->input);
			key_state_flag = 0;
		}
	#endif			
	}
#endif
	input_sync(ts->input);
	ts->prev_touches = touches;
}


static int startup_chip(struct i2c_client *client)
{
	u8 tmp = 0x00;
	int rc = gsl_ts_write(client, 0xe0, &tmp, 1);
	msleep(10);
	return rc;	
}


static int reset_chip(struct i2c_client *client)
{
	u8 buf[4] = {0x00};
	u8 tmp = 0x88;
	int rc = gsl_ts_write(client, 0xe0, &tmp, sizeof(tmp));
	if (rc < 0) 
		return rc;
	msleep(10);

	tmp = 0x04;
	rc = gsl_ts_write(client, 0xe4, &tmp, sizeof(tmp));
	if (rc < 0) 
		return rc;
	msleep(10);

	rc = gsl_ts_write(client, 0xbc, buf, sizeof(buf));
	if (rc < 0) 
		return rc;
	msleep(10);
	
	return 0;
}


static void gsl_ts_xy_worker(struct work_struct *work)
{
	int rc;
	u8 read_buf[4] = {0};
	struct gsl_ts *ts = container_of(work, struct gsl_ts,work);

	//pr_info("---gsl_ts_xy_worker---\n");

	if (ts->is_suspended == true) {
		dev_dbg(&ts->client->dev, "TS is supended\n");
		ts->int_pending = true;
		goto schedule;
	}

	/* read data from DATA_REG */
	rc = gsl_ts_read(ts->client, 0x80, ts->touch_data, ts->dd->data_size);
	//pr_info("---touches: %d ---\n",ts->touch_data[0]);

	if (rc < 0) 
	{
		dev_err(&ts->client->dev, "read failed\n");
		goto schedule;
	}

	if (ts->touch_data[ts->dd->touch_index] == 0xff) {
		goto schedule;
	}

	rc = gsl_ts_read( ts->client, 0xbc, read_buf, sizeof(read_buf));
	if (rc < 0) 
	{
		dev_err(&ts->client->dev, "read 0xbc failed\n");
		goto schedule;
	}
		

	if (read_buf[3] == 0 && read_buf[2] == 0 && read_buf[1] == 0 && read_buf[0] == 0)
	{
		process_gslX680_data(ts);
	}
	else
	{
		rc = reset_chip(ts->client);
		if (rc < 0) {
			dev_err(&ts->client->dev, "%s: reset_chip failed\n", __func__);
			goto schedule;
		}
		rc = startup_chip(ts->client);
		if (rc < 0) {
			dev_err(&ts->client->dev, "%s: startup_chip failed\n", __func__);
			goto schedule;
		}
	}
	
schedule:
	enable_irq(SW_INT_IRQNO_PIO);
		
}
/*
 * It sucks this is duplicated for each driver right now.
*/
static int _fetch_sysconfig_para(void)
{
	int ret = -1;
	int ctp_used = -1;
	char name[I2C_NAME_SIZE];
	__u32 twi_addr = 0;
	//__u32 twi_id = 0;
	script_parser_value_type_t type = SCRIPT_PARSER_VALUE_TYPE_STRING;

	pr_info("%s. \n", __func__);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_used", &ctp_used, 1)){
		pr_err("%s: script_parser_fetch err. \n", __func__);
		goto script_parser_fetch_err;
	}
	if(1 != ctp_used){
		pr_err("%s: ctp_unused. \n",  __func__);
		//ret = 1;
		return ret;
	}

	if(SCRIPT_PARSER_OK != script_parser_fetch_ex("ctp_para", "ctp_name", (int *)(&name), &type, sizeof(name)/sizeof(int))){
		pr_err("%s: script_parser_fetch err. \n", __func__);
		goto script_parser_fetch_err;
	}
	if(strcmp(GSLX680_NAME, name)){
		pr_err("%s: name %s does not match CTP_NAME. \n", __func__, name);
		pr_err(GSLX680_NAME);
		//ret = 1;
		return ret;
	}

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_twi_addr", &twi_addr, sizeof(twi_addr)/sizeof(__u32))){
		pr_err("%s: script_parser_fetch err. \n", name);
		goto script_parser_fetch_err;
	}
	//big-endian or small-endian?
	//pr_info("%s: before: ctp_twi_addr is 0x%x, dirty_addr_buf: 0x%hx. dirty_addr_buf[1]: 0x%hx \n", __func__, twi_addr, u_i2c_addr.dirty_addr_buf[0], u_i2c_addr.dirty_addr_buf[1]);
	u_i2c_addr.dirty_addr_buf[0] = twi_addr;
	u_i2c_addr.dirty_addr_buf[1] = I2C_CLIENT_END;
	pr_info("%s: after: ctp_twi_addr is 0x%x, dirty_addr_buf: 0x%hx. dirty_addr_buf[1]: 0x%hx \n", __func__, twi_addr, u_i2c_addr.dirty_addr_buf[0], u_i2c_addr.dirty_addr_buf[1]);
	//pr_info("%s: after: ctp_twi_addr is 0x%x, u32_dirty_addr_buf: 0x%hx. u32_dirty_addr_buf[1]: 0x%hx \n", __func__, twi_addr, u32_dirty_addr_buf[0],u32_dirty_addr_buf[1]);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_twi_id", &twi_id, sizeof(twi_id)/sizeof(__u32))){
		pr_err("%s: script_parser_fetch err. \n", name);
		goto script_parser_fetch_err;
	}
	pr_info("%s: ctp_twi_id is %d. \n", __func__, twi_id);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_screen_max_x", &screen_max_x, 1)){
		pr_err("%s: script_parser_fetch err. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: screen_max_x = %d. \n", __func__, screen_max_x);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_screen_max_y", &screen_max_y, 1)){
		pr_err("%s: script_parser_fetch err. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: screen_max_y = %d. \n", __func__, screen_max_y);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_revert_x_flag", &revert_x_flag, 1)){
		pr_err("%s: script_parser_fetch err. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: revert_x_flag = %d. \n", __func__, revert_x_flag);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_revert_y_flag", &revert_y_flag, 1)){
		pr_err("%s: script_parser_fetch err. \n", __func__);
		goto script_parser_fetch_err;
	}
	pr_info("%s: revert_y_flag = %d. \n", __func__, revert_y_flag);

	if(SCRIPT_PARSER_OK != script_parser_fetch("ctp_para", "ctp_exchange_x_y_flag", &exchange_x_y_flag, 1)){
		pr_err("ft5x_ts: script_parser_fetch err. \n");
		goto script_parser_fetch_err;
	}
	pr_info("%s: exchange_x_y_flag = %d. \n", __func__, exchange_x_y_flag);

	return 0;

script_parser_fetch_err:
	pr_notice("=========script_parser_fetch_err============\n");
	return ret;
}

static void _free_platform_resource(void)
{
	if(gpio_addr){
		iounmap(gpio_addr);
	}

	if(gpio_int_hdle){
		gpio_release(gpio_int_hdle, 2);
	}

	if(gpio_wakeup_hdle){
		gpio_release(gpio_wakeup_hdle, 2);
	}
	
	if(gpio_io_hdle){
		gpio_release(gpio_io_hdle, 2);
	}

	return;
}


static int _init_platform_resource(void)
{
	int ret = 0;

	gpio_addr = ioremap(PIO_BASE_ADDRESS, PIO_RANGE_SIZE);
	//pr_info("%s, gpio_addr = 0x%x. \n", __func__, gpio_addr);
	if(!gpio_addr) {
		ret = -EIO;
		goto exit_ioremap_failed;
	}
	gpio_wakeup_hdle = gpio_request_ex("ctp_para", "ctp_wakeup");
	if(!gpio_wakeup_hdle) {
		pr_warning("%s: tp_wakeup request gpio fail!\n", __func__);

	}
	gpio_io_hdle = gpio_request_ex("ctp_para", "ctp_io_port");
	if(!gpio_io_hdle) {
		pr_warning("%s: tp_io request gpio fail!\n", __func__);
	}
	return ret;

exit_ioremap_failed:
	_free_platform_resource();
	return ret;
}


static void ctp_clear_penirq(void)
{
	int reg_val;
	//clear the IRQ_EINT29 interrupt pending
	//pr_info("clear pend irq pending\n");
	reg_val = readl(gpio_addr + PIO_INT_STAT_OFFSET);
	//writel(reg_val,gpio_addr + PIO_INT_STAT_OFFSET);
	//writel(reg_val&(1<<(IRQ_EINT21)),gpio_addr + PIO_INT_STAT_OFFSET);
	if((reg_val = (reg_val&(1<<(CTP_IRQ_NO))))){
		pr_info("==CTP_IRQ_NO=\n");
		writel(reg_val,gpio_addr + PIO_INT_STAT_OFFSET);
	}
	return;
}

static int ctp_set_irq_mode(char *major_key, char *subkey, ext_int_mode int_mode)
{
	int ret = 0;
	__u32 reg_num = 0;
	__u32 reg_addr = 0;
	__u32 reg_val = 0;
	//config gpio to int mode
	pr_info("%s: config gpio to int mode. \n", __func__);
#ifndef SYSCONFIG_GPIO_ENABLE
#else
	if(gpio_int_hdle){
		gpio_release(gpio_int_hdle, 2);
	}
	gpio_int_hdle = gpio_request_ex(major_key, subkey);
	if(!gpio_int_hdle){
		pr_info("request tp_int_port failed. \n");
		ret = -1;
		goto request_tp_int_port_failed;
	}
	gpio_get_one_pin_status(gpio_int_hdle, gpio_int_info, subkey, 1);
	pr_info("%s, %d: gpio_int_info, port = %d, port_num = %d. \n", __func__, __LINE__, \
		gpio_int_info[0].port, gpio_int_info[0].port_num);
#endif

#ifdef AW_GPIO_INT_API_ENABLE
#else
	pr_info(" INTERRUPT CONFIG\n");
	reg_num = (gpio_int_info[0].port_num)%8;
	reg_addr = (gpio_int_info[0].port_num)/8;
	reg_val = readl(gpio_addr + int_cfg_addr[reg_addr]);
	reg_val &= (~(7 << (reg_num * 4)));
	reg_val |= (int_mode << (reg_num * 4));
	writel(reg_val,gpio_addr+int_cfg_addr[reg_addr]);

	ctp_clear_penirq();

	reg_val = readl(gpio_addr+PIO_INT_CTRL_OFFSET);
	reg_val |= (1 << (gpio_int_info[0].port_num));
	writel(reg_val,gpio_addr+PIO_INT_CTRL_OFFSET);

	udelay(1);
#endif

request_tp_int_port_failed:
	return ret;
}


static int ctp_judge_int_occur(void)
{
	//int reg_val[3];
	int reg_val;
	int ret = -1;

	reg_val = readl(gpio_addr + PIO_INT_STAT_OFFSET);
	if(reg_val&(1<<(CTP_IRQ_NO))){
		ret = 0;
	}
	return ret;
}

static irqreturn_t gsl_ts_irq(int irq, void *dev_id)
{	
	struct gsl_ts *ts = dev_id;

	//pr_info("==========GSLX680 Interrupt============\n");

	if(ctp_judge_int_occur()){
		pr_info("Other Interrupt\n");
		return IRQ_NONE;
	}

	if (ts->is_suspended == true) 
		return IRQ_HANDLED;	
		
	disable_irq_nosync(SW_INT_IRQNO_PIO);
	if (!work_pending(&ts->work)) 
	{
		queue_work(ts->wq, &ts->work);
	}
		
	return IRQ_HANDLED;
}

static const struct i2c_device_id gslx680_ts_id[] = {
	{ GSLX680_NAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, gslx680_ts_id);

static int gsl_ts_init_ts(struct i2c_client *client, struct gsl_ts *ts)
{
	struct input_dev *input_device;
	int rc = 0;
	
	printk("[GSLX680] Enter %s\n", __func__);

	
	ts->dd = &devices[ts->device_id];

	if (ts->device_id == 0) {
		ts->dd->data_size = MAX_FINGERS * ts->dd->touch_bytes + ts->dd->touch_meta_data;
		ts->dd->touch_index = 0;
	}

	ts->touch_data = kzalloc(ts->dd->data_size, GFP_KERNEL);
	if (!ts->touch_data) {
		pr_err("%s: Unable to allocate memory\n", __func__);
		return -ENOMEM;
	}

	ts->prev_touches = 0;

	input_device = input_allocate_device();
	if (!input_device) {
		rc = -ENOMEM;
		goto error_alloc_dev;
	}

	ts->input = input_device;
	input_device->name = GSLX680_NAME;
	input_device->id.bustype = BUS_I2C;
	input_device->dev.parent = &client->dev;
	input_set_drvdata(input_device, ts);

#ifdef REPORT_DATA_ANDROID_4_0
	__set_bit(EV_ABS, input_device->evbit);
	__set_bit(EV_KEY, input_device->evbit);
	__set_bit(EV_REP, input_device->evbit);
	__set_bit(INPUT_PROP_DIRECT, input_device->propbit);
	input_mt_init_slots(input_device, (MAX_CONTACTS+1));
#else
	input_set_abs_params(input_device,ABS_MT_TRACKING_ID, 0, (MAX_CONTACTS+1), 0, 0);
	set_bit(EV_ABS, input_device->evbit);
	set_bit(EV_KEY, input_device->evbit);
	
#endif

	set_bit(ABS_X, input_device->absbit);
	set_bit(ABS_Y, input_device->absbit);
	set_bit(ABS_PRESSURE, input_device->absbit);
	set_bit(BTN_TOUCH, input_device->keybit);

	input_set_abs_params(input_device,
			ABS_X, 0, SCREEN_MAX_X, 0, 0);
	input_set_abs_params(input_device,
			ABS_Y, 0, SCREEN_MAX_Y, 0, 0);
	input_set_abs_params(input_device,
			ABS_PRESSURE, 0, PRESS_MAX, 0 , 0);


#ifdef HAVE_TOUCH_KEY
	input_device->evbit[0] = BIT_MASK(EV_KEY);
	for (i = 0; i < MAX_KEY_NUM; i++)
		set_bit(key_array[i], input_device->keybit);
#endif

	set_bit(ABS_MT_POSITION_X, input_device->absbit);
	set_bit(ABS_MT_POSITION_Y, input_device->absbit);
	set_bit(ABS_MT_TOUCH_MAJOR, input_device->absbit);
	set_bit(ABS_MT_WIDTH_MAJOR, input_device->absbit);

	input_set_abs_params(input_device,ABS_MT_POSITION_X, 0, SCREEN_MAX_X, 0, 0);
	input_set_abs_params(input_device,ABS_MT_POSITION_Y, 0, SCREEN_MAX_Y, 0, 0);
	input_set_abs_params(input_device,ABS_MT_TOUCH_MAJOR, 0, PRESS_MAX, 0, 0);
	input_set_abs_params(input_device,ABS_MT_WIDTH_MAJOR, 0, 200, 0, 0);


	rc = ctp_set_irq_mode("ctp_para", "ctp_int_port", CTP_IRQ_MODE);
	if(0 != rc){
		pr_info("%s:ctp_set_irq_mode err.\n", __func__);
		goto exit_set_irq_mode;
	}

	ts->wq = create_singlethread_workqueue("kworkqueue_ts");
	if (!ts->wq) {
		dev_err(&client->dev, "Could not create workqueue\n");
		goto error_wq_create;
	}
	flush_workqueue(ts->wq);	

	INIT_WORK(&ts->work, gsl_ts_xy_worker);

	rc = input_register_device(input_device);
	if (rc)
		goto error_unreg_device;

	client->irq = SW_INT_IRQNO_PIO;

	return 0;
error_unreg_device:
	destroy_workqueue(ts->wq);
error_wq_create:
	input_free_device(input_device);
exit_set_irq_mode:
	//enable_irq(SW_INT_IRQNO_PIO);
error_alloc_dev:
	kfree(ts->touch_data);
	return rc;
}



static int gslX680_chip_init(void)
{
	if(EGPIO_SUCCESS != gpio_write_one_pin_value(gpio_wakeup_hdle, 1, "ctp_wakeup")){
		pr_info("%s: err when operate wake gpio. \n", __func__);
		return -EIO;
	}
	
    gpio_init_status |= (1 << 0);
    
    
	if(EGPIO_SUCCESS != gpio_set_one_pin_pull(gpio_int_hdle, 1, "ctp_io_port")){
		pr_info("%s: err when operate int gpio. \n", __func__);
		return -EIO;
	}
    gpio_init_status |= (1 << 1);

	msleep(20);
	return 0;
}


static int gsl_write_interface(struct i2c_client *client, const u8 reg, u8 *buf, u32 num)
{
	struct i2c_msg xfer_msg[1];

	buf[0] = reg;

	xfer_msg[0].addr = client->addr;
	xfer_msg[0].len = num + 1;
	xfer_msg[0].flags = client->flags & I2C_M_TEN;
	xfer_msg[0].buf = buf;

	return i2c_transfer(client->adapter, xfer_msg, 1) == 1 ? 0 : -EFAULT;
}

static __inline__ void fw2buf(u8 *buf, const u32 *fw)
{
	u32 *u32_buf = (int *)buf;
	*u32_buf = *fw;
}


static int gsl_load_fw(struct i2c_client *client)
{
	u8 buf[DMA_TRANS_LEN*4 + 1] = {0};
	u8 send_flag = 1;
	u8 *cur = buf + 1;
	u32 source_line = 0;
	u32 source_len = ARRAY_SIZE(GSLX680_FW);
	int rc;

	printk("=============gsl_load_fw start==============\n");

	for (source_line = 0; source_line < source_len; source_line++) 
	{
		/* init page trans, set the page val */
		if (GSL_PAGE_REG == GSLX680_FW[source_line].offset)
		{
			fw2buf(cur, &GSLX680_FW[source_line].val);
			rc = gsl_write_interface(client, GSL_PAGE_REG, buf, 4);
			if (rc < 0) {
				pr_info("%s: gsl_write_interface failed. \n", __func__);
				return rc;
			}
			send_flag = 1;
		}
		else 
		{
			if (1 == send_flag % (DMA_TRANS_LEN < 0x20 ? DMA_TRANS_LEN : 0x20))
	    			buf[0] = (u8)GSLX680_FW[source_line].offset;

			fw2buf(cur, &GSLX680_FW[source_line].val);
			cur += 4;

			if (0 == send_flag % (DMA_TRANS_LEN < 0x20 ? DMA_TRANS_LEN : 0x20)) 
			{
	    			rc = gsl_write_interface(client, buf[0], buf, cur - buf - 1);
					if (rc < 0) {
						pr_info("%s: gsl_write_interface failed. \n", __func__);
						return rc;
					}
	    			cur = buf + 1;
			}

			send_flag++;
		}
	}

	printk("=============gsl_load_fw end==============\n");
	return 0;
}


static int gslX680_shutdown_low(void)
{
	if(EGPIO_SUCCESS != gpio_write_one_pin_value(gpio_wakeup_hdle, 0, "ctp_wakeup")){
		pr_info("%s: err when operate wake gpio. \n", __func__);
		return -EIO;
	}
	return 0;
}

static int gslX680_shutdown_high(void)
{
	if(EGPIO_SUCCESS != gpio_write_one_pin_value(gpio_wakeup_hdle, 1, "ctp_wakeup")){
		pr_info("%s: err when operate wake gpio. \n", __func__);
		return -EIO;
	}
	return 0;
}

static int init_chip(struct i2c_client *client)
{
	int rc;
	rc = reset_chip(client);
	if (rc < 0) {
		pr_info("%s: reset_chip fail: %i\n", __func__, rc);
		return rc;
	}
	rc = gsl_load_fw(client);	
	if (rc < 0) {
		pr_info("%s: gsl_load_fw fail: %i\n", __func__, rc);
		return rc;
	}		
	rc = startup_chip(client);
	if (rc < 0) {
		pr_info("%s: startup_chip fail: %i\n", __func__, rc);
		return rc;
	}
	rc = reset_chip(client);
	if (rc < 0) {
		pr_info("%s: second reset_chip fail: %i\n", __func__, rc);
		return rc;
	}
	rc = gslX680_shutdown_low();
	if (rc < 0) {
		pr_info("%s: gslX680_shutdown_low fail: %i\n", __func__, rc);
		return rc;
	}
	msleep(50); 	
	rc = gslX680_shutdown_high();
	if (rc < 0) {
		pr_info("%s: gslX680_shutdown_high fail: %i\n", __func__, rc);
		return rc;
	}
	msleep(30); 		
	rc = gslX680_shutdown_low();
	if (rc < 0) {
		pr_info("%s: second gslX680_shutdown_low fail: %i\n", __func__, rc);
		return rc;
	}
	msleep(5); 	
	rc = gslX680_shutdown_high();
	if (rc < 0) {
		pr_info("%s: second gslX680_shutdown_high fail: %i\n", __func__, rc);
		return rc;
	}
	msleep(20); 	
	rc = reset_chip(client);
	if (rc < 0) {
		pr_info("%s: third reset_chip fail: %i\n", __func__, rc);
		return rc;
	}
	rc = startup_chip(client);
	if (rc < 0) {
		pr_info("%s: second startup_chip fail: %i\n", __func__, rc);
		return rc;
	}
	return 0;
}


static int check_mem_data_init(struct i2c_client *client)
{
	char write_buf;
	char read_buf[4]  = {0};
	int rc;
	msleep(30);
	write_buf = 0x00;
	rc = gsl_ts_write(client,0xf0, &write_buf, sizeof(write_buf));
	if (rc < 0) {
		pr_info("%s: gsl_ts_write fail: %i\n", __func__, rc);
		return rc;
	}
	rc = gsl_ts_read(client,0x00, read_buf, sizeof(read_buf));
	if (rc < 0) {
		pr_info("%s: gsl_ts_read 1 fail: %i\n", __func__, rc);
		return rc;
	}
	rc = gsl_ts_read(client,0x00, read_buf, sizeof(read_buf));
	if (rc < 0) {
		pr_info("%s: gsl_ts_read 2 fail: %i\n", __func__, rc);
		return rc;
	}
	if (read_buf[3] != 0x1 || read_buf[2] != 0 || read_buf[1] != 0 || read_buf[0] != 0)
	{
		pr_info("!!!!!!!!!!!page: %x offset: %x val: %x %x %x %x\n",0x0, 0x0, read_buf[3], read_buf[2], read_buf[1], read_buf[0]);
		return -1;
	}
	
	return 0;
}


static int
gslx680_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct gsl_ts *ts;
	int rc;

	pr_info("====%s begin=====.  \n", __func__);
	
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C functionality not supported\n");
		return -ENODEV;
	}

	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	pr_info("==kzalloc success=\n");

	ts->client = client;
	i2c_set_clientdata(client, ts);
	ts->device_id = id->driver_data;

	ts->touch_data = NULL;
	ts->is_suspended = false;
	ts->int_pending = false;
	mutex_init(&ts->sus_lock);

	rc = gsl_ts_init_ts(client, ts);
	if (rc < 0) {
		dev_err(&client->dev, "GSLX680 init failed\n");
		goto error_mutex_destroy;
	}

	rc = gslX680_chip_init();
	if (rc < 0) {
		dev_err(&client->dev, "gslX680_chip_init failed\n");
		goto error_mutex_destroy;
	}
	
	rc = init_chip(ts->client);
	if (rc < 0) {
		dev_err(&client->dev, "init_chip failed\n");
		goto error_mutex_destroy;
	}
	
	if (check_mem_data_init(ts->client) == -1) {
		dev_err(&client->dev, "GSLX680 init gpio failed\n");
	    goto error_mutex_destroy;
	}
	
	rc = request_irq(SW_INT_IRQNO_PIO, gsl_ts_irq, IRQF_TRIGGER_FALLING | IRQF_SHARED, "gslx680", ts);

	if (rc < 0) {
		dev_err(&client->dev, "gslx680_ts probe: request irq failed\n");
		goto error_mutex_destroy;
	}
	
	pr_info("==%s over =\n", __func__);
	return 0;
error_mutex_destroy:
	mutex_destroy(&ts->sus_lock);
	input_free_device(ts->input);
	kfree(ts);
	return rc;
}

int ctp_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;

	if(twi_id == adapter->nr)
	{
		pr_info("%s: Detected chip %s at adapter %d, address 0x%02x\n",
			 __func__, GSLX680_NAME, i2c_adapter_id(adapter), client->addr);

		strlcpy(info->type, GSLX680_NAME, I2C_NAME_SIZE);
		return 0;
	}else{
		return -ENODEV;
	}
}


static int __devexit gslx680_ts_remove(struct i2c_client *client)
{
	struct gsl_ts *ts = i2c_get_clientdata(client);
	pr_info("==gslx680_ts_remove=\n");
	
	disable_irq_nosync(SW_INT_IRQNO_PIO);
	cancel_work_sync(&ts->work);
	free_irq(SW_INT_IRQNO_PIO, ts);
	input_unregister_device(ts->input);
	input_free_device(ts->input);
	destroy_workqueue(ts->wq);

	mutex_destroy(&ts->sus_lock);

	//device_remove_file(&ts->input->dev, &dev_attr_debug_enable);
	
	if (ts->touch_data)
		kfree(ts->touch_data);
	kfree(ts);
	
	return 0;
}


static struct i2c_driver gslx680_ts_driver = {
	.class = I2C_CLASS_HWMON,
	.probe		= gslx680_ts_probe,
	.remove		= __devexit_p(gslx680_ts_remove),
	.id_table	= gslx680_ts_id,
	.driver	= {
		.name	= GSLX680_NAME,
		.owner	= THIS_MODULE,
	},
	.address_list	= u_i2c_addr.normal_i2c,
	.detect = ctp_detect
};


static int __init gslx680_ts_init(void)
{
	int ret = 0;

	pr_info("===========================%s=====================\n", __func__);
	ret = _fetch_sysconfig_para();
	if(ret < 0){
		pr_info("_fetch_sysconfig_para failed.\n");
		return -1;
	}
	ret = _init_platform_resource();
	if(ret < 0){
		pr_info("_init_platform_resource failed.\n");
		return -1;
	}
	
	ret = i2c_add_driver(&gslx680_ts_driver);

	return ret;
}

static void __exit gslx680_ts_exit(void)
{
	pr_info("==gslx680_ts_exit==\n");
	_free_platform_resource();
	i2c_del_driver(&gslx680_ts_driver);
}


late_initcall(gslx680_ts_init);
module_exit(gslx680_ts_exit);


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GSLX680 touchscreen controller driver");
MODULE_AUTHOR("Joe Burmeister, joe.a.burmeister@gmail.com");
MODULE_ALIAS("platform:gsl_ts");

#include "network.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "box.h"
#include "demo.h"
#include "option_list.h"
#include "blas.h"
#include "locations.h"
#include <stdio.h>

static int coco_ids[] = {1,2,3,4,5,6,7,8,9,10,11,13,14,15,16,17,18,19,20,21,22,23,24,25,27,28,31,32,33,34,35,36,37,38,39,40,41,42,43,44,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,67,70,72,73,74,75,76,77,78,79,80,81,82,84,85,86,87,88,89,90};

void train_detector(char *datacfg, char *cfgfile, char *weightfile, int *gpus, int ngpus, int clear)
{
    list *options = read_data_cfg(datacfg);
    char *train_images = option_find_str(options, "train", "data/train.list");//其中的option_find_str函数的意思是先找“train”关键字的值，没有的话默认使用"data/train.list"
    char *backup_directory = option_find_str(options, "backup", "/backup/");

    srand(time(0));//重置随机数种子，以当前时间为参数
    char *base = basecfg(cfgfile);//去除cfgfile中的路径，只保留后面cfgfile自己的名称
    //printf("%s\n", base);
    float avg_loss = -1;
    network *nets = calloc(ngpus, sizeof(network));//给一个network类型的数组分配空间（其中network结构体包含的是训练网络的参数之类，epoch，subdivisions等，还有网络的总层数n（这边的层数不同于学术上的层数，代码中真正的网络的层数是YOLO-voc.cfg中除了[net]以外有[]标志的），layer的数组layers，cost，目前训练到第多少批的seen）

    srand(time(0));
    int seed = rand();
    int i;
    for(i = 0; i < ngpus; ++i){
        srand(seed);
#ifdef GPU
        cuda_set_device(gpus[i]);
#endif
        nets[i] = parse_network_cfg(cfgfile);//去解析网络结构，每个GPU中都会去创建一个网络，包括网络的训练参数，有多少层，各层的类别，参数，还有各层的输入大小应该是什么之类
        //给网络加载权重，由于
        if(weightfile){
            load_weights(&nets[i], weightfile);
        }
        if(clear) *nets[i].seen = 0;
        nets[i].learning_rate *= ngpus;//？？？
    }
    srand(time(0));
    network net = nets[0];

    int imgs = net.batch * net.subdivisions * ngpus;//一次载入到显存的图片数量  
    //printf("Learning Rate: %g, Momentum: %g, Decay: %g\n", net.learning_rate, net.momentum, net.decay);
    data train, buffer;//定义两个data类型的变量，每个中应该是都包含检测框的信息

    layer l = net.layers[net.n - 1];//我之前居然去找，还以为这里怎么net.n不是net的层数了，居然忘记索引是从0开始的！！

    int classes = l.classes;
    float jitter = l.jitter;

    list *plist = get_paths(train_images);
    //int N = plist->size;
    char **paths = (char **)list_to_array(plist);//变成char的二维数组的吧

    load_args args = {0};//将要加载的数据参数
    args.w = net.w;
    args.h = net.h;
    args.paths = paths;
    args.n = imgs;//一次加载的数据量
    args.m = plist->size;//总的数据量
    args.classes = classes;
    args.jitter = jitter;
    args.num_boxes = l.max_boxes;//默认是30个
    args.d = &buffer;
    args.type = DETECTION_DATA;
    args.threads = 8;

    args.angle = net.angle;
    args.exposure = net.exposure;
    args.saturation = net.saturation;
    args.hue = net.hue;

    pthread_t load_thread = load_data(args);//返回线程ID，去创建一个执行什么的线程，但是暂时还没有启动这个线程吧。load_data去加载数据的，load_data创建多线程，并且去执行中的load_threads，我之前一直怀疑这样简单去执行一个多线程有啥影响，load_threads又没有个返回值，后来有才发现其实这些参数都是结构体，乍看不是指针，但是里面却包含指针的成员函数，后面就算给结构体赋值给形参，只要对形参的指针的指向内容做改变，这就都改变了。这边可以就认为多线程也就只是去选择在某个时候执行函数罢了。
    clock_t time;
    int count = 0;
    //while(i*imgs < N*120){
    while(get_current_batch(net) < net.max_batches){
        if(l.random && count++%10 == 0){//l.random决定是否要多尺度训练，如果多尺度训练的话，每10batches
            //printf("Resizing\n");
            //这边将图片缩放的范围是320-608，可以根据我自己原本的图像的大小更改10,10,32这三个数
            int dim = (rand() % 10 + 10) * 32;
            //在最后的200batches中，把所有图片都缩放到608（32的19倍数）
            if (get_current_batch(net)+200 > net.max_batches) dim = 608;
            //int dim = (rand() % 4 + 16) * 32;
            //printf("%d\n", dim);
            //这边是将网络的输入大小也改成dim
            args.w = dim;
            args.h = dim;
            //线程相关 
            pthread_join(load_thread, 0);//在main函数结束前我们得想办法让程序停下来，pthread_join方法的功能就是等待线程结束，要等的线程就是第一个参数，程序会在这个地方停下来，直到线程结束（否则事先主线程停下来之后，程序所有的资源都会没有了，包括线程），第二个参数用来接受线程函数的返回值，是void**类型的指针，如果没有返回值，就直接设为NULL吧。这边之后，完成了随机选取n张图片（注意：这边批梯度下降法去训练网络每次选取的图片并不是一开始把图打乱，然后一批一批挨个选，而是每次都会把图打乱，然后每次抽取n张），每张图会resize到网络需要的大小（当然前面有多尺度训练，后面会改网络大小），然后会将args.d，即buffer中的X与y赋值，X包含整个这一批resize之后的图像，y则包含这一批图像的框的信息（都是一行对应一张图片，这边得注意的是一张图会给设定一个框数量的最大值）
            train = buffer;
            free_data(train);
            load_thread = load_data(args);//这句好像没用啊
            //放缩网络的大小进行训练 
            for(i = 0; i < ngpus; ++i){
                resize_network(nets + i, dim, dim);
            }
            net = nets[0];
        }
        //这边是不进行多尺度训练的话
        time=clock();
        pthread_join(load_thread, 0);
        train = buffer;
        load_thread = load_data(args);

        /*
        int k;
        for(k = 0; k < l.max_boxes; ++k){
            box b = float_to_box(train.y.vals[10] + 1 + k*5);
            if(!b.x) break;
            printf("loaded: %f %f %f %f\n", b.x, b.y, b.w, b.h);
        }
        */
        /*
        int zz;
        for(zz = 0; zz < train.X.cols; ++zz){
            image im = float_to_image(net.w, net.h, 3, train.X.vals[zz]);
            int k;
            for(k = 0; k < l.max_boxes; ++k){
                box b = float_to_box(train.y.vals[zz] + k*5);
                printf("%f %f %f %f\n", b.x, b.y, b.w, b.h);
                draw_bbox(im, b, 1, 1,0,0);
            }
            show_image(im, "truth11");
            cvWaitKey(0);
            save_image(im, "truth11");
        }
        */

        printf("Loaded: %lf seconds\n", sec(clock()-time));

        time=clock();
        float loss = 0;
//训练网络 
#ifdef GPU
        if(ngpus == 1){
            loss = train_network(net, train);//
        } else {
            loss = train_networks(nets, ngpus, train, 4);
        }
#else
        loss = train_network(net, train);
#endif
        if (avg_loss < 0) avg_loss = loss;
        avg_loss = avg_loss*.9 + loss*.1;

        i = get_current_batch(net);
        printf("%d: %f, %f avg, %f rate, %lf seconds, %d images\n", get_current_batch(net), loss, avg_loss, get_current_rate(net), sec(clock()-time), i*imgs);
        if(i%1000==0){
#ifdef GPU
            if(ngpus != 1) sync_nets(nets, ngpus, 0);
#endif
            char buff[256];
            sprintf(buff, "%s/%s.backup", backup_directory, base);
            save_weights(net, buff);
        }
        if(i%1000==0 || (i < 1000 && i%200 == 0)){
#ifdef GPU
            if(ngpus != 1) sync_nets(nets, ngpus, 0);
#endif
            char buff[256];
            sprintf(buff, "%s/%s_%d.weights", backup_directory, base, i);
            save_weights(net, buff);
        }
        free_data(train);
    }
#ifdef GPU
    if(ngpus != 1) sync_nets(nets, ngpus, 0);
#endif
    char buff[256];
    sprintf(buff, "%s/%s_final.weights", backup_directory, base);
    save_weights(net, buff);
}


static int get_coco_image_id(char *filename)
{
    char *p = strrchr(filename, '_');
    return atoi(p+1);
}

static void print_cocos(FILE *fp, char *image_path, box *boxes, float **probs, int num_boxes, int classes, int w, int h)
{
    int i, j;
    int image_id = get_coco_image_id(image_path);
    for(i = 0; i < num_boxes; ++i){
        float xmin = boxes[i].x - boxes[i].w/2.;
        float xmax = boxes[i].x + boxes[i].w/2.;
        float ymin = boxes[i].y - boxes[i].h/2.;
        float ymax = boxes[i].y + boxes[i].h/2.;

        if (xmin < 0) xmin = 0;
        if (ymin < 0) ymin = 0;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        float bx = xmin;
        float by = ymin;
        float bw = xmax - xmin;
        float bh = ymax - ymin;

        for(j = 0; j < classes; ++j){
            if (probs[i][j]) fprintf(fp, "{\"image_id\":%d, \"category_id\":%d, \"bbox\":[%f, %f, %f, %f], \"score\":%f},\n", image_id, coco_ids[j], bx, by, bw, bh, probs[i][j]);
        }
    }
}

void print_detector_detections(FILE **fps, char *id, box *boxes, float **probs, int total, int classes, int w, int h)
{
    int i, j;
    for(i = 0; i < total; ++i){
        float xmin = boxes[i].x - boxes[i].w/2. + 1;
        float xmax = boxes[i].x + boxes[i].w/2. + 1;
        float ymin = boxes[i].y - boxes[i].h/2. + 1;
        float ymax = boxes[i].y + boxes[i].h/2. + 1;

        if (xmin < 1) xmin = 1;
        if (ymin < 1) ymin = 1;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        for(j = 0; j < classes; ++j){
            if (probs[i][j]) fprintf(fps[j], "%s %f %f %f %f %f\n", id, probs[i][j],
                    xmin, ymin, xmax, ymax);
        }
    }
}

void print_imagenet_detections(FILE *fp, int id, box *boxes, float **probs, int total, int classes, int w, int h)
{
    int i, j;
    for(i = 0; i < total; ++i){
        float xmin = boxes[i].x - boxes[i].w/2.;
        float xmax = boxes[i].x + boxes[i].w/2.;
        float ymin = boxes[i].y - boxes[i].h/2.;
        float ymax = boxes[i].y + boxes[i].h/2.;

        if (xmin < 0) xmin = 0;
        if (ymin < 0) ymin = 0;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        for(j = 0; j < classes; ++j){
            int class = j;
            if (probs[i][class]) fprintf(fp, "%d %d %f %f %f %f %f\n", id, j+1, probs[i][class],
                    xmin, ymin, xmax, ymax);
        }
    }
}

void validate_detector_flip(char *datacfg, char *cfgfile, char *weightfile, char *outfile)
{
    int j;
    list *options = read_data_cfg(datacfg);
    char *valid_images = option_find_str(options, "valid", "data/train.list");
    char *name_list = option_find_str(options, "names", "data/names.list");
    char *prefix = option_find_str(options, "results", "results");
    char **names = get_labels(name_list);
    char *mapf = option_find_str(options, "map", 0);
    int *map = 0;
    if (mapf) map = read_map(mapf);

    network net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 2);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net.learning_rate, net.momentum, net.decay);
    srand(time(0));

    list *plist = get_paths(valid_images);
    char **paths = (char **)list_to_array(plist);

    layer l = net.layers[net.n-1];
    int classes = l.classes;

    char buff[1024];
    char *type = option_find_str(options, "eval", "voc");
    FILE *fp = 0;
    FILE **fps = 0;
    int coco = 0;
    int imagenet = 0;
    if(0==strcmp(type, "coco")){
        if(!outfile) outfile = "coco_results";
        snprintf(buff, 1024, "%s/%s.json", prefix, outfile);
        fp = fopen(buff, "w");
        fprintf(fp, "[\n");
        coco = 1;
    } else if(0==strcmp(type, "imagenet")){
        if(!outfile) outfile = "imagenet-detection";
        snprintf(buff, 1024, "%s/%s.txt", prefix, outfile);
        fp = fopen(buff, "w");
        imagenet = 1;
        classes = 200;
    } else {
        if(!outfile) outfile = "comp4_det_test_";
        fps = calloc(classes, sizeof(FILE *));
        for(j = 0; j < classes; ++j){
            snprintf(buff, 1024, "%s/%s%s.txt", prefix, outfile, names[j]);
            fps[j] = fopen(buff, "w");
        }
    }


    box *boxes = calloc(l.w*l.h*l.n, sizeof(box));
    float **probs = calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = calloc(classes+1, sizeof(float *));

    int m = plist->size;
    int i=0;
    int t;

    float thresh = .005;
    float nms = .45;

    int nthreads = 4;
    image *val = calloc(nthreads, sizeof(image));
    image *val_resized = calloc(nthreads, sizeof(image));
    image *buf = calloc(nthreads, sizeof(image));
    image *buf_resized = calloc(nthreads, sizeof(image));
    pthread_t *thr = calloc(nthreads, sizeof(pthread_t));

    image input = make_image(net.w, net.h, net.c*2);

    load_args args = {0};
    args.w = net.w;
    args.h = net.h;
    //args.type = IMAGE_DATA;
    args.type = LETTERBOX_DATA;

    for(t = 0; t < nthreads; ++t){
        args.path = paths[i+t];
        args.im = &buf[t];
        args.resized = &buf_resized[t];
        thr[t] = load_data_in_thread(args);
    }
    time_t start = time(0);
    for(i = nthreads; i < m+nthreads; i += nthreads){
        fprintf(stderr, "%d\n", i);
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            pthread_join(thr[t], 0);
            val[t] = buf[t];
            val_resized[t] = buf_resized[t];
        }
        for(t = 0; t < nthreads && i+t < m; ++t){
            args.path = paths[i+t];
            args.im = &buf[t];
            args.resized = &buf_resized[t];
            thr[t] = load_data_in_thread(args);
        }
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            char *path = paths[i+t-nthreads];
            char *id = basecfg(path);
            copy_cpu(net.w*net.h*net.c, val_resized[t].data, 1, input.data, 1);
            flip_image(val_resized[t]);
            copy_cpu(net.w*net.h*net.c, val_resized[t].data, 1, input.data + net.w*net.h*net.c, 1);

            network_predict(net, input.data);
            int w = val[t].w;
            int h = val[t].h;
            get_region_boxes(l, w, h, net.w, net.h, thresh, probs, boxes, 0, map, .5, 0);
            if (nms) do_nms_sort(boxes, probs, l.w*l.h*l.n, classes, nms);
            if (coco){
                print_cocos(fp, path, boxes, probs, l.w*l.h*l.n, classes, w, h);
            } else if (imagenet){
                print_imagenet_detections(fp, i+t-nthreads+1, boxes, probs, l.w*l.h*l.n, classes, w, h);
            } else {
                print_detector_detections(fps, id, boxes, probs, l.w*l.h*l.n, classes, w, h);
            }
            free(id);
            free_image(val[t]);
            free_image(val_resized[t]);
        }
    }
    for(j = 0; j < classes; ++j){
        if(fps) fclose(fps[j]);
    }
    if(coco){
        fseek(fp, -2, SEEK_CUR); 
        fprintf(fp, "\n]\n");
        fclose(fp);
    }
    fprintf(stderr, "Total Detection Time: %f Seconds\n", (double)(time(0) - start));
}


void validate_detector(char *datacfg, char *cfgfile, char *weightfile, char *outfile)
{
    int j;
    list *options = read_data_cfg(datacfg);
    char *valid_images = option_find_str(options, "valid", "data/train.list");
    char *name_list = option_find_str(options, "names", "data/names.list");
    char *prefix = option_find_str(options, "results", "results");
    char **names = get_labels(name_list);
    char *mapf = option_find_str(options, "map", 0);
    int *map = 0;
    if (mapf) map = read_map(mapf);

    network net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net.learning_rate, net.momentum, net.decay);
    srand(time(0));

    list *plist = get_paths(valid_images);
    char **paths = (char **)list_to_array(plist);

    layer l = net.layers[net.n-1];
    int classes = l.classes;

    char buff[1024];
    char *type = option_find_str(options, "eval", "voc");
    FILE *fp = 0;
    FILE **fps = 0;
    int coco = 0;
    int imagenet = 0;
    if(0==strcmp(type, "coco")){
        if(!outfile) outfile = "coco_results";
        snprintf(buff, 1024, "%s/%s.json", prefix, outfile);
        fp = fopen(buff, "w");
        fprintf(fp, "[\n");
        coco = 1;
    } else if(0==strcmp(type, "imagenet")){
        if(!outfile) outfile = "imagenet-detection";
        snprintf(buff, 1024, "%s/%s.txt", prefix, outfile);
        fp = fopen(buff, "w");
        imagenet = 1;
        classes = 200;
    } else {
        if(!outfile) outfile = "comp4_det_test_";
        fps = calloc(classes, sizeof(FILE *));
        for(j = 0; j < classes; ++j){
            snprintf(buff, 1024, "%s/%s%s.txt", prefix, outfile, names[j]);
            fps[j] = fopen(buff, "w");
        }
    }


    box *boxes = calloc(l.w*l.h*l.n, sizeof(box));
    float **probs = calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = calloc(classes+1, sizeof(float *));

    int m = plist->size;
    int i=0;
    int t;

    float thresh = .005;
    float nms = .45;

    int nthreads = 4;
    image *val = calloc(nthreads, sizeof(image));
    image *val_resized = calloc(nthreads, sizeof(image));
    image *buf = calloc(nthreads, sizeof(image));
    image *buf_resized = calloc(nthreads, sizeof(image));
    pthread_t *thr = calloc(nthreads, sizeof(pthread_t));

    load_args args = {0};
    args.w = net.w;
    args.h = net.h;
    //args.type = IMAGE_DATA;
    args.type = LETTERBOX_DATA;

    for(t = 0; t < nthreads; ++t){
        args.path = paths[i+t];
        args.im = &buf[t];
        args.resized = &buf_resized[t];
        thr[t] = load_data_in_thread(args);
    }
    time_t start = time(0);
    for(i = nthreads; i < m+nthreads; i += nthreads){
        fprintf(stderr, "%d\n", i);
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            pthread_join(thr[t], 0);
            val[t] = buf[t];
            val_resized[t] = buf_resized[t];
        }
        for(t = 0; t < nthreads && i+t < m; ++t){
            args.path = paths[i+t];
            args.im = &buf[t];
            args.resized = &buf_resized[t];
            thr[t] = load_data_in_thread(args);
        }
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            char *path = paths[i+t-nthreads];
            char *id = basecfg(path);
            float *X = val_resized[t].data;
            network_predict(net, X);
            int w = val[t].w;
            int h = val[t].h;
            get_region_boxes(l, w, h, net.w, net.h, thresh, probs, boxes, 0, map, .5, 0);
            if (nms) do_nms_sort(boxes, probs, l.w*l.h*l.n, classes, nms);
            if (coco){
                print_cocos(fp, path, boxes, probs, l.w*l.h*l.n, classes, w, h);
            } else if (imagenet){
                print_imagenet_detections(fp, i+t-nthreads+1, boxes, probs, l.w*l.h*l.n, classes, w, h);
            } else {
                print_detector_detections(fps, id, boxes, probs, l.w*l.h*l.n, classes, w, h);
            }
            free(id);
            free_image(val[t]);
            free_image(val_resized[t]);
        }
    }
    for(j = 0; j < classes; ++j){
        if(fps) fclose(fps[j]);
    }
    if(coco){
        fseek(fp, -2, SEEK_CUR); 
        fprintf(fp, "\n]\n");
        fclose(fp);
    }
    fprintf(stderr, "Total Detection Time: %f Seconds\n", (double)(time(0) - start));
}

void validate_detector_recall(char *cfgfile, char *weightfile)
{
    network net = parse_network_cfg(cfgfile);
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net.learning_rate, net.momentum, net.decay);
    srand(time(0));

    list *plist = get_paths("/home/echo/EXERCISEs/airplaneDetec/infrared_val.txt");
    char **paths = (char **)list_to_array(plist);

    layer l = net.layers[net.n-1];
    int classes = l.classes;

    int j, k;
    box *boxes = calloc(l.w*l.h*l.n, sizeof(box));
    float **probs = calloc(l.w*l.h*l.n, sizeof(float *));
    for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = calloc(classes+1, sizeof(float *));

    int m = plist->size;
    int i=0;

    float thresh = .25;
    float iou_thresh = .5;
    float nms = .4;

    int total = 0;
    int correct = 0;
    int proposals = 0;
    float avg_iou = 0;

    for(i = 0; i < m; ++i){
        char *path = paths[i];
        image orig = load_image_color(path, 0, 0);
        image sized = resize_image(orig, net.w, net.h);
        char *id = basecfg(path);
        network_predict(net, sized.data);
        get_region_boxes(l, sized.w, sized.h, net.w, net.h, thresh, probs, boxes, 1, 0, .5, 1);
        if (nms) do_nms(boxes, probs, l.w*l.h*l.n, 1, nms);

        char labelpath[4096];
        find_replace(path, "images", "labels", labelpath);
        find_replace(labelpath, "JPEGImages", "labels", labelpath);
        find_replace(labelpath, ".jpg", ".txt", labelpath);
        find_replace(labelpath, ".JPEG", ".txt", labelpath);

        int num_labels = 0;
        box_label *truth = read_boxes(labelpath, &num_labels);
        for(k = 0; k < l.w*l.h*l.n; ++k){
            if(probs[k][0] > thresh){
                ++proposals;
            }
        }
        for (j = 0; j < num_labels; ++j) {
            ++total;
            box t = {truth[j].x, truth[j].y, truth[j].w, truth[j].h};
            float best_iou = 0;
            for(k = 0; k < l.w*l.h*l.n; ++k){
                float iou = box_iou(boxes[k], t);
                if(probs[k][0] > thresh && iou > best_iou){
                    best_iou = iou;
                }
            }
            avg_iou += best_iou;
            if(best_iou > iou_thresh){
                ++correct;
            }
        }

        //fprintf(stderr, "%5d %5d %5d\tRPs/Img: %.2f\tIOU: %.2f%%\tRecall:%.2f%%\n", i, correct, total, (float)proposals/(i+1), avg_iou*100/total, 100.*correct/total);
        fprintf(stderr, "ID:%5d Correct:%5d Total:%5d\tRPs/Img: %.2f\tIOU: %.2f%%\tRecall:%.2f%%\t", i, correct, total, (float)proposals/(i+1), avg_iou*100/total, 100.*correct/total);
	fprintf(stderr, "proposals:%5d\tPrecision:%.2f%%\n",proposals,100.*correct/(float)proposals);
        free(id);
        free_image(orig);
        free_image(sized);
    }
}

void test_detector(char *datacfg, char *cfgfile, char *weightfile, char *filename, float thresh, float hier_thresh, char *outfile, int fullscreen)
{
    list *options = read_data_cfg(datacfg);
    char *name_list = option_find_str(options, "names", "data/names.list");
    char **names = get_labels(name_list);

    image **alphabet = load_alphabet();
    network net = parse_network_cfg(cfgfile);//
    if(weightfile){
        load_weights(&net, weightfile);
    }
    set_batch_network(&net, 1);
    srand(2222222);
    clock_t time;
    char buff[256];
    char *input = buff;
    int j;
    float nms=.2;
    int step = 60;//步长
    int crop_flag = 1;
    while(1){
        if(filename){
            strncpy(input, filename, 256);//filename中的前256个字符拷贝至input
        }else {
            printf("Enter Image Path: ");
            fflush(stdout);
            input = fgets(input, 256, stdin);
            if(!input) return;
            strtok(input, "\n");
        }
        image im = load_image_color(input,0,0);//原模原样导入图片
        layer l = net.layers[net.n-1];//l.batch=1
        //以下是我写的测试代码
        int wn,hn;
        int **location;//记录的是每一个图像块的min_x,max_x.min_y,max_y
        if(im.w<=352){
            wn=1;
        }else if((im.w-352)%step==0){
            wn=(im.w-352)/step+1;
        }else{
            wn = ceil(((float)im.w-352)/step)+1;
 	 //hn = ceil(((float)im.h-352)/step)+1;
        }
        if(im.h<=352){
            hn=1;
        }else if((im.h-352)%step==0){
            hn=(im.h-352)/step+1;
        }else{
            hn = ceil(((float)im.h-352)/step)+1;
        }
        
        printf("wn:%d,hn:%d\n",wn,hn);
        location =(int **)calloc(wn*hn, sizeof(int *));//堆起来的二维数组
        for(j = 0;j<wn*hn;j++){
            location[j] = calloc(4, sizeof(int));
        }

        //此处处理一共多少张图
	image sized;
        if(crop_flag){
            image im_patch;
	    //image **sizeds;
            get_patchLocations(im, location, step, wn, hn);
            int box_i,probs_i;
	    //以上的目的是获得image **sizeds以及int **location;
/*
            printf("FILE\n");
            FILE *fp = fopen("../location.txt","w");
            int whN_i,location_i;
	    //printf("location[0][0]:%d\n",location[0][0]);
            for(whN_i=0;whN_i<wn*hn;++whN_i){
               //printf("test here7\n");
               for(location_i = 0;location_i<4;++location_i){
                  //printf("test here8\n");
                  fprintf(fp,"%d ",location[whN_i][location_i]);
                  //printf("test here9\n");	
	       }
               fprintf(fp,"\n");
            }
            fclose(fp);
*/
//最后想要得到的是boxes和probs

            int dx,dy;
            float *X;
            box *boxes_all = calloc(l.w*l.h*l.n*wn*hn, sizeof(box));
            float **probs_all = calloc(l.w*l.h*l.n*wn*hn, sizeof(float *));
            for(j = 0; j < l.w*l.h*l.n*wn*hn; ++j) probs_all[j] = calloc(l.classes + 1, sizeof(float *));
            box *boxes = calloc(l.w*l.h*l.n, sizeof(box));
            float **probs = calloc(l.w*l.h*l.n, sizeof(float *));
            for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = calloc(l.classes + 1, sizeof(float *));
            for(j = 0;j<wn*hn;j++){
               dx = location[j][0];
               dy = location[j][2];
               im_patch = crop_image(im, dx, dy, 352, 352);
               sized = letterbox_image(im_patch, net.w, net.h);
               X = sized.data;//
               time=clock();
               network_predict(net, X);
               printf("%s: Predicted in %f seconds.\n", input, sec(clock()-time));
               get_region_boxes(l, im_patch.w, im_patch.h, net.w, net.h, thresh, probs, boxes, 0, 0, hier_thresh, 1);//就是将预测的结果填到boxes与probs中去
               for(box_i=0;box_i<l.w*l.h*l.n;box_i++){
                  printf("box_i:%d\n",box_i);
                  boxes_all[j*l.w*l.h*l.n+box_i].x = (boxes[box_i].x*im_patch.w+dx)/im.w;
                
                  boxes_all[j*l.w*l.h*l.n+box_i].w = (boxes[box_i].w*im_patch.w)/im.w;
                  boxes_all[j*l.w*l.h*l.n+box_i].y = (boxes[box_i].y*im_patch.h+dy)/im.h;
                  boxes_all[j*l.w*l.h*l.n+box_i].h = (boxes[box_i].h*im_patch.h)/im.h;
                  for(probs_i=0;probs_i<l.classes + 1;probs_i++){
                     printf("lalalala\n");
                     printf("%f\n",probs[box_i][2]);
                     probs_all[j*l.w*l.h*l.n+box_i][probs_i] = probs[box_i][probs_i];
                     printf("probs_i:%d\n",probs_i);
                  }
               }

            }
            if (nms) do_nms_obj(boxes_all, probs_all, l.w*l.h*l.n*wn*hn, l.classes, nms);//nms=0.4
        //else if (nms) do_nms_sort(boxes, probs, l.w*l.h*l.n, l.classes, nms);
            draw_detections(im, l.w*l.h*l.n*wn*hn, thresh, boxes_all, probs_all, names, alphabet, l.classes);
            free(boxes_all);
            //free_ptrs((void **)probs, l.w*l.h*l.n);
            free_ptrs((void **)probs_all, l.w*l.h*l.n*wn*hn);
        }else{
            sized = letterbox_image(im, net.w, net.h);//反正是先同比例的缩小放大，再就是生成与网络相同尺寸的图片（貌似用不够的话则用常数补充的来着）
            //image sized = resize_image(im, net.w, net.h);
            //image sized2 = resize_max(im, net.w);
            //image sized = crop_image(sized2, -((net.w - sized2.w)/2), -((net.h - sized2.h)/2), net.w, net.h);
            //resize_network(&net, sized.w, sized.h);
            //layer l = net.layers[net.n-1];//l.batch=1
            box *boxes = calloc(l.w*l.h*l.n, sizeof(box));
            float **probs = calloc(l.w*l.h*l.n, sizeof(float *));//二维数组，要预测的框的个数为行数
            for(j = 0; j < l.w*l.h*l.n; ++j) probs[j] = calloc(l.classes + 1, sizeof(float *));//每一行给分配去判定属于这（classes + 1）类的概率
            float *X = sized.data;//
            time=clock();
            network_predict(net, X);
            printf("%s: Predicted in %f seconds.\n", input, sec(clock()-time));
            get_region_boxes(l, im.w, im.h, net.w, net.h, thresh, probs, boxes, 0, 0, hier_thresh, 1);//就是将预测的结果填到boxes与probs中去
//由此可见boxes中的x,y,w,h都是相对于整张图像的比例
/*
            FILE *fp2 = fopen("boxes.txt","w");
            for(j=0;j<l.w*l.h*l.n;j++){
            　　　fprintf(fp2,"%f %f %f %f\n",boxes[j].x,boxes[j].y,boxes[j].w,boxes[j].h);
        　　　　}
        　　　　fclose(fp2);
*/
            if (nms) do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms); //nms=0.4
            //else if (nms) do_nms_sort(boxes, probs, l.w*l.h*l.n, l.classes, nms);
            draw_detections(im, l.w*l.h*l.n, thresh, boxes, probs, names, alphabet, l.classes);
            free(boxes);
            free_ptrs((void **)probs, l.w*l.h*l.n);
        }
	   
        if(outfile){
            save_image(im, outfile);
        }else{
            save_image(im, "predictions");
#ifdef OPENCV
            cvNamedWindow("predictions", CV_WINDOW_NORMAL); 
            if(fullscreen){
                cvSetWindowProperty("predictions", CV_WND_PROP_FULLSCREEN, CV_WINDOW_FULLSCREEN);
            }
            show_image(im, "predictions");
            cvWaitKey(0);
            cvDestroyAllWindows();
#endif
        }

        free_image(im);
        free_image(sized);
        //free(boxes);
        if (filename) break;
    }
}

void run_detector(int argc, char **argv)
{
    //以下都是去寻找命令行中设置的参数，没有的话则默认
    char *prefix = find_char_arg(argc, argv, "-prefix", 0);//
    float thresh = find_float_arg(argc, argv, "-thresh", .24);
    float hier_thresh = find_float_arg(argc, argv, "-hier", .5);
    int cam_index = find_int_arg(argc, argv, "-c", 0);
    int frame_skip = find_int_arg(argc, argv, "-s", 0);
    int avg = find_int_arg(argc, argv, "-avg", 3);
    //提示用法
    if(argc < 4){
        fprintf(stderr, "usage: %s %s [train/test/valid] [cfg] [weights (optional)]\n", argv[0], argv[1]);
        return;
    }
    char *gpu_list = find_char_arg(argc, argv, "-gpus", 0);
    char *outfile = find_char_arg(argc, argv, "-out", 0);
    int *gpus = 0;//指向gpu
    int gpu = 0;
    int ngpus = 0;//gpu的个数
    if(gpu_list){
        printf("%s\n", gpu_list);
        int len = strlen(gpu_list);
        ngpus = 1;
        int i;
        for(i = 0; i < len; ++i){
            if (gpu_list[i] == ',') ++ngpus;
        }
        gpus = calloc(ngpus, sizeof(int));
        for(i = 0; i < ngpus; ++i){
            gpus[i] = atoi(gpu_list);
            gpu_list = strchr(gpu_list, ',')+1;
        }
    } else {
        gpu = gpu_index;
        gpus = &gpu;
        ngpus = 1;
    }

    int clear = find_arg(argc, argv, "-clear");
    int fullscreen = find_arg(argc, argv, "-fullscreen");
    int width = find_int_arg(argc, argv, "-w", 0);
    int height = find_int_arg(argc, argv, "-h", 0);
    int fps = find_int_arg(argc, argv, "-fps", 0);

    char *datacfg = argv[3];
    char *cfg = argv[4];
    char *weights = (argc > 5) ? argv[5] : 0;
    char *filename = (argc > 6) ? argv[6]: 0;
    if(0==strcmp(argv[2], "test")) test_detector(datacfg, cfg, weights, filename, thresh, hier_thresh, outfile, fullscreen);
    else if(0==strcmp(argv[2], "train")) train_detector(datacfg, cfg, weights, gpus, ngpus, clear);
    else if(0==strcmp(argv[2], "valid")) validate_detector(datacfg, cfg, weights, outfile);
    else if(0==strcmp(argv[2], "valid2")) validate_detector_flip(datacfg, cfg, weights, outfile);
    else if(0==strcmp(argv[2], "recall")) validate_detector_recall(cfg, weights);
    else if(0==strcmp(argv[2], "demo")) {
        list *options = read_data_cfg(datacfg);
        int classes = option_find_int(options, "classes", 20);
        char *name_list = option_find_str(options, "names", "data/names.list");
        char **names = get_labels(name_list);
        demo(cfg, weights, thresh, cam_index, filename, names, classes, frame_skip, prefix, avg, hier_thresh, width, height, fps, fullscreen);
    }
}
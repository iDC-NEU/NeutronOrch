#include "core/neutronstar.hpp"
#include "core/ntsPeerRPC.hpp"
class GCN_SAMPLE_PD_impl {
public:
  int iterations;
  ValueType learn_rate;
  ValueType weight_decay;
  ValueType drop_rate;
  ValueType alpha;
  ValueType beta1;
  ValueType beta2;
  ValueType epsilon;
  ValueType decay_rate;
  ValueType decay_epoch;

  // graph
  VertexSubset *active;
  Graph<Empty> *graph;
  //std::vector<CSC_segment_pinned *> subgraphs;
  // NN
  GNNDatum *gnndatum;
  NtsVar L_GT_C;
  NtsVar L_GT_G;
  NtsVar MASK;
  NtsVar MASK_gpu;
  //GraphOperation *gt;
  Cuda_Stream* cuda_stream;
  std::vector<at::cuda::CUDAStream> torch_stream;

  nts::ctx::NtsContext *ctx;
  // Variables
  std::vector<Parameter *> P;
  std::vector<NtsVar> X;
  NtsVar Y_PD;
  NtsVar F;
  NtsVar loss;
  NtsVar tt;
  torch::nn::Dropout drpmodel;
  FullyRepGraph* fully_rep_graph;
  float acc;
  int batch;
  long correct;
  int pipeline_num;

  double training_time = 0;
  double training_cpu_time = 0;
  double exec_time = 0;
  double transfer_sample_time = 0;
  double transfer_feature_time = 0;
  double gather_feature_time = 0;
  double sample_time = 0;
  double all_sync_time = 0;
  double sync_time = 0;
  double all_graph_sync_time = 0;
  double graph_sync_time = 0;
  double all_compute_time = 0;
  double compute_time = 0;
  double all_copy_time = 0;
  double copy_time = 0;
  double graph_time = 0;
  double all_graph_time = 0;


    std::mutex sample_mutex;
    std::mutex transfer_mutex;
    std::mutex train_mutex;

  GCN_SAMPLE_PD_impl(Graph<Empty> *graph_, int iterations_,
                        bool process_local = false,
                        bool process_overlap = false) {
    graph = graph_;
    iterations = iterations_;

    active = graph->alloc_vertex_subset();
    active->fill();

    graph->init_gnnctx(graph->config->layer_string);
    graph->init_gnnctx_fanout(graph->config->fanout_string);
    graph->init_rtminfo();
    graph->rtminfo->set(graph->config);
    //        graph->rtminfo->process_local = graph->config->process_local;
    //        graph->rtminfo->reduce_comm = graph->config->process_local;
    //        graph->rtminfo->lock_free=graph->config->lock_free;
    //        graph->rtminfo->process_overlap = graph->config->overlap;

    graph->rtminfo->with_weight = true;
    graph->rtminfo->with_cuda = true;
    graph->rtminfo->copy_data = false;

    pipeline_num = 1;
  }
  void init_graph() {

    fully_rep_graph=new FullyRepGraph(graph);
    fully_rep_graph->GenerateAll();
    fully_rep_graph->SyncAndLog("read_finish");
    
    graph->init_message_buffer();
    graph->init_communicatior();
    ctx=new nts::ctx::NtsContext();
  }
  void init_nn() {

    learn_rate = graph->config->learn_rate;
    weight_decay = graph->config->weight_decay;
    drop_rate = graph->config->drop_rate;
    alpha = graph->config->learn_rate;
    decay_rate = graph->config->decay_rate;
    decay_epoch = graph->config->decay_epoch;
    beta1 = 0.9;
    beta2 = 0.999;
    epsilon = 1e-9;




    gnndatum = new GNNDatum(graph->gnnctx, graph);
    if (0 == graph->config->feature_file.compare("random")) {
      gnndatum->random_generate();
    } else {
      gnndatum->readFeature_Label_Mask(graph->config->feature_file,
                                       graph->config->label_file,
                                       graph->config->mask_file);
    }
    gnndatum->registLabel(L_GT_C);
    gnndatum->registMask(MASK);
    gnndatum->genereate_gpu_data();
    // L_GT_G = L_GT_C.cuda();
    MASK_gpu = MASK.cuda();

    for (int i = 0; i < graph->gnnctx->layer_size.size() - 1; i++) {
      P.push_back(new Parameter(graph->gnnctx->layer_size[i], graph->gnnctx->layer_size[i + 1], alpha, beta1, beta2, epsilon, weight_decay));
      //            P.push_back(new Parameter(graph->gnnctx->layer_size[i],
      //                        graph->gnnctx->layer_size[i+1]));
    }

    torch::Device GPU(torch::kCUDA, 0);
    for (int i = 0; i < P.size(); i++) {
      P[i]->init_parameter();
      P[i]->set_decay(decay_rate, decay_epoch);
      P[i]->to(GPU);
      P[i]->Adam_to_GPU();
    }

    drpmodel = torch::nn::Dropout(
        torch::nn::DropoutOptions().p(drop_rate).inplace(false));

    //        F=graph->Nts->NewOnesTensor({graph->gnnctx->l_v_num,
    //        graph->gnnctx->layer_size[0]},torch::DeviceType::CPU);

    F = graph->Nts->NewLeafTensor(
        gnndatum->local_feature,
        {graph->gnnctx->l_v_num, graph->gnnctx->layer_size[0]},
        torch::DeviceType::CPU);

    for (int i = 0; i < graph->gnnctx->layer_size.size(); i++) {
      NtsVar d;
      X.push_back(d);
    }
    // X[0]=F.cuda().set_requires_grad(true);
     X[0] = F.set_requires_grad(true);
  }

  long getCorrect(NtsVar &input, NtsVar &target) {
    // NtsVar predict = input.log_softmax(1).argmax(1);
    NtsVar predict = input.argmax(1);
    NtsVar output = predict.to(torch::kLong).eq(target).to(torch::kLong);
    return output.sum(0).item<long>();
  }

  void shuffle_vec(std::vector<VertexId>& vec) {
    unsigned seed = std::chrono::system_clock::now ().time_since_epoch ().count();
    std::shuffle (vec.begin(), vec.end(), std::default_random_engine(seed));
  }
  
  void Test(long s) { // 0 train, //1 eval //2 test
    NtsVar mask_train = MASK_gpu.eq(s);
    NtsVar all_train =
        X[graph->gnnctx->layer_size.size() - 1]
            .argmax(1)
            .to(torch::kLong)
            .eq(L_GT_G)
            .to(torch::kLong)
            .masked_select(mask_train.view({mask_train.size(0)}));
    NtsVar all = all_train.sum(0).cpu();
    long *p_correct = all.data_ptr<long>();
    long g_correct = 0;
    long p_train = all_train.size(0);
    long g_train = 0;
    MPI_Datatype dt = get_mpi_data_type<long>();
    MPI_Allreduce(p_correct, &g_correct, 1, dt, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(&p_train, &g_train, 1, dt, MPI_SUM, MPI_COMM_WORLD);
    float acc_train = 0.0;
    if (g_train > 0)
      acc_train = float(g_correct) / g_train;
    if (graph->partition_id == 0) {
      if (s == 0)
        std::cout << "Train ACC: " << acc_train << " " << g_train << " "
                  << g_correct << std::endl;
      else if (s == 1)
        std::cout << "Eval  ACC: " << acc_train << " " << g_train << " "
                  << g_correct << " " << std::endl;
      else if (s == 2)
        std::cout << "Test  ACC: " << acc_train << " " << g_train << " "
                  << g_correct << " " << std::endl;
    }
  }

  void Loss(NtsVar &left,NtsVar &right) {
    //  return torch::nll_loss(a,L_GT_C);
    torch::Tensor a = left.log_softmax(1);
    //torch::Tensor mask_train = MASK_gpu.eq(0);
    loss = torch::nll_loss(a,right);
    if (ctx->training == true) {
      ctx->appendNNOp(left, loss);
    }    
  }

  void Update() {
    for (int i = 0; i < P.size(); i++) {
      P[i]->all_reduce_to_gradient(P[i]->W.grad().cpu());
      P[i]->learn_local_with_decay_Adam();
      P[i]->next();
    }
  }

  NtsVar vertexForward(NtsVar &a, NtsVar &x) {
    NtsVar y;
    int layer = graph->rtminfo->curr_layer;
    if (layer == 1) {
      y = P[layer]->forward(a);
      y = y.log_softmax(1); //CUDA

    } else if (layer == 0) {
      //y = P[layer]->forward(torch::relu(drpmodel(a)));
      y = torch::dropout(torch::relu(P[layer]->forward(a)), drop_rate, ctx->is_train());
    }
    return y;
  }

  void Forward(FastSampler* sampler, int type=0) {
      graph->rtminfo->forward = true;
      SampledSubgraph *sg;
      correct = 0;
      batch = 0;
      NtsVar target_lab;
      target_lab=graph->Nts->NewLabelTensor({graph->config->batch_size}, torch::DeviceType::CUDA);
      // TODO: 流水线设计如下，分为三个线程，一个CPU线程进行聚合，进行一个进行传输，一个进行GPU训练

      while(sampler->sample_not_finished()){
           sample_time -= get_time();
           sg=sampler->sample_fast(graph->config->batch_size, pipeline_num - 1);
           sample_time += get_time();
           gather_feature_time -= get_time();
          // X[0]=nts::op::get_feature(sg->sampled_sgs[graph->gnnctx->layer_size.size()-2]->src(),F,graph);
           gather_feature_time += get_time();
           training_cpu_time -= get_time();
           graph->rtminfo->curr_layer = 0; 
        //    X[0] = X[0].cuda();
        //    NtsVar Y_i=ctx->runGraphOp<nts::op::SingleGPUSampleGraphOp>(sg,graph,1,X[0]);
           //NtsVar Y_tmp = ctx->runGraphOp<nts::op::PushDownOp>(sg,graph,1,F);
           //nts::op::init_embedding(Y_tmp, gnndatum->local_embedding,graph);
           ctx->runGraphOp<nts::op::PushDownOp>(sg,graph,gnndatum->local_embedding,1,F);
           training_cpu_time += get_time(); 
           transfer_feature_time -= get_time();
           sampler->load_label_gpu(target_lab,gnndatum->dev_local_label);
           //Y_i = Y_i.cuda();
           at::cuda::setCurrentCUDAStream(torch_stream[pipeline_num - 1]);
           NtsVar Y_i = graph->Nts->NewKeyTensor({sg->sampled_sgs[0]->src_size,F.size(1)},torch::DeviceType::CUDA); 
           sampler->load_embedding_gpu_local(Y_i, gnndatum->dev_local_embedding);
           transfer_feature_time += get_time();
           training_time -= get_time();
           X[1] = ctx->runVertexForward([&](NtsVar n_i,NtsVar v_i){
                        return vertexForward(n_i, v_i);
                      },
                    Y_i,
                    X[0]);
           for(int l=1;l<(graph->gnnctx->layer_size.size()-1);l++){//forward
               graph->rtminfo->curr_layer = l;
               int hop=(graph->gnnctx->layer_size.size()-2)-l;
               
               NtsVar Y_i=ctx->runGraphOp<nts::op::SingleGPUSampleGraphOp>(sg,graph,hop,X[l],cuda_stream);
               X[l + 1] = ctx->runVertexForward([&](NtsVar n_i,NtsVar v_i){
                        return vertexForward(n_i, v_i);
                      },
                    Y_i,
                    X[l]);
              
           }
           
          Loss(X[graph->gnnctx->layer_size.size()-1],target_lab);
          if (ctx->training) {
            ctx->self_backward(false);
            Update();
            for (int i = 0; i < P.size(); i++) {
              P[i]->zero_grad();
            }
          }
          training_time += get_time();
          
           correct += getCorrect(X[graph->gnnctx->layer_size.size()-1], target_lab);
           batch++;

    }
       sampler->restart();
      acc = 1.0 * correct / sampler->work_range[1];
      if (type == 0) {
        LOG_INFO("Train Acc: %f %d %d", acc, correct, sampler->work_range[1]);
      } else if (type == 1) {
        LOG_INFO("Eval Acc: %f %d %d", acc, correct, sampler->work_range[1]);
      } else if (type == 2) {
        LOG_INFO("Test Acc: %f %d %d", acc, correct, sampler->work_range[1]);
      }
    // loss=X[graph->gnnctx->layer_size.size()-1];
    // #run_time=1784.571165(s)
    // exec_time=1842.431756(s) reddit
    
    // run_time=2947.184987(s) cpu
    // exec_time=2986.859283(s)
  }

  void ForwardPipeline(FastSampler* sampler, int type=0){
      graph->rtminfo->forward = true;
      correct = 0;
      batch = 0;
//      SampledSubgraph *sg;
//      NtsVar target_lab;
//      target_lab=graph->Nts->NewLabelTensor({graph->config->batch_size}, torch::DeviceType::CUDA);

      SampledSubgraph *sg[pipeline_num];
      NtsVar tmp_X0[pipeline_num];
      NtsVar tmp_target_lab[pipeline_num];
      for(int i = 0; i < pipeline_num; i++) {
          tmp_X0[i] = graph->Nts->NewLeafTensor({1000,F.size(1)},
                                                torch::DeviceType::CUDA);
          tmp_target_lab[i] = graph->Nts->NewLabelTensor({graph->config->batch_size},
                                                         torch::DeviceType::CUDA);
      }

        std::atomic_flag is_aggregate = ATOMIC_FLAG_INIT;

        std::printf("pipeline num: %d\n", pipeline_num);
      std::thread threads[pipeline_num];
      for(int tid = 0; tid < pipeline_num; tid++) {
          threads[tid] = std::thread([&](int thread_id) {
            std::unique_lock<std::mutex> sample_lock(sample_mutex, std::defer_lock);
            sample_lock.lock();
              while (sampler->sample_not_finished()) {
                  sample_time -= get_time();
                  sg[thread_id] = sampler->sample_fast(graph->config->batch_size,  thread_id);
//                  cudaStreamSynchronize(cuda_stream[thread_id].stream);
                  sample_time += get_time();
                  sample_lock.unlock();

                  std::unique_lock<std::mutex> transfer_lock(transfer_mutex, std::defer_lock);
                  transfer_lock.lock();
                  gather_feature_time -= get_time();
                  // X[0]=nts::op::get_feature(sg->sampled_sgs[graph->gnnctx->layer_size.size()-2]->src(),F,graph);
                  gather_feature_time += get_time();
                  training_cpu_time -= get_time();
                  graph->rtminfo->curr_layer = 0;
                  //    X[0] = X[0].cuda();
                  //    NtsVar Y_i=ctx->runGraphOp<nts::op::SingleGPUSampleGraphOp>(sg,graph,1,X[0]);
                  //NtsVar Y_tmp = ctx->runGraphOp<nts::op::PushDownOp>(sg,graph,1,F);
                  //nts::op::init_embedding(Y_tmp, gnndatum->local_embedding,graph);
                  if(!is_aggregate.test_and_set()){
                      ctx->runGraphOp<nts::op::PushDownOp>(sg[thread_id], graph, gnndatum->local_embedding, 1, F);
                  }
                  training_cpu_time += get_time();
                  transfer_feature_time -= get_time();
                  sampler->load_label_gpu(tmp_target_lab[thread_id], gnndatum->dev_local_label);
                  //Y_i = Y_i.cuda();
                  at::cuda::setCurrentCUDAStream(torch_stream[thread_id]);
                  NtsVar Y_i = graph->Nts->NewKeyTensor({sg[thread_id]->sampled_sgs[0]->src_size, F.size(1)}, torch::DeviceType::CUDA);
                  sampler->load_embedding_gpu_local(&cuda_stream[thread_id], sg[thread_id], Y_i, gnndatum->dev_local_embedding);
                  transfer_feature_time += get_time();
//                  cudaStreamSynchronize(cuda_stream[thread_id].stream);
                  transfer_lock.unlock();

                  std::unique_lock<std::mutex> train_lock(train_mutex, std::defer_lock);
                  train_lock.lock();
                  training_time -= get_time();
                  at::cuda::setCurrentCUDAStream(torch_stream[thread_id]);
                  X[1] = ctx->runVertexForward([&](NtsVar n_i, NtsVar v_i) {
                                                   return vertexForward(n_i, v_i);
                                               },
                                               Y_i,
                                               tmp_X0[thread_id]);
                  for (int l = 1; l < (graph->gnnctx->layer_size.size() - 1); l++) {//forward
                      graph->rtminfo->curr_layer = l;
                      int hop = (graph->gnnctx->layer_size.size() - 2) - l;

                      NtsVar Y_i = ctx->runGraphOp<nts::op::SingleGPUSampleGraphOp>(sg[thread_id], graph, hop, X[l], &cuda_stream[thread_id]);
                      X[l + 1] = ctx->runVertexForward([&](NtsVar n_i, NtsVar v_i) {
                                                           return vertexForward(n_i, v_i);
                                                       },
                                                       Y_i,
                                                       X[l]);

                  }

                  Loss(X[graph->gnnctx->layer_size.size() - 1], tmp_target_lab[thread_id]);
                  if (ctx->training) {
                      ctx->self_backward(false);
                      Update();
                      for (int i = 0; i < P.size(); i++) {
                          P[i]->zero_grad();
                      }
                  }
                  training_time += get_time();

                  correct += getCorrect(X[graph->gnnctx->layer_size.size() - 1], tmp_target_lab[thread_id]);
                  batch++;
//                  cudaStreamSynchronize(cuda_stream[thread_id].stream);
                  train_lock.unlock();
                  sample_lock.lock();
              }
              sample_lock.unlock();
          }, tid);
      }

      for(int i = 0; i < pipeline_num; i++) {
          threads[i].join();
      }
      sampler->restart();
      acc = 1.0 * correct / sampler->work_range[1];
      if (type == 0) {
          LOG_INFO("Train Acc: %f %d %d", acc, correct, sampler->work_range[1]);
      } else if (type == 1) {
          LOG_INFO("Eval Acc: %f %d %d", acc, correct, sampler->work_range[1]);
      } else if (type == 2) {
          LOG_INFO("Test Acc: %f %d %d", acc, correct, sampler->work_range[1]);
      }

  }

  void run() {
    if (graph->partition_id == 0)
      printf("GNNmini::Engine[Dist.GPU.GCNimpl] running [%d] Epochs\n",
             iterations);

    //      graph->print_info();
    std::vector<VertexId> train_nids, val_nids, test_nids;
      for (int i = 0; i < graph->gnnctx->l_v_num; ++i) {
        int type = gnndatum->local_mask[i];
        if (type == 0) {
          train_nids.push_back(i);
        } else if (type == 1) {
          val_nids.push_back(i);
        } else if (type == 2) {
          test_nids.push_back(i);
        }
    }

    //layer 0 graphop push down
    PartitionedGraph *partitioned_graph;
    partitioned_graph = new PartitionedGraph(graph, active);
    partitioned_graph->GenerateAll([&](VertexId src, VertexId dst) {
      return nts::op::nts_norm_degree(graph,src, dst);
    },CPU_T);
    ctx->eval();
    Y_PD = ctx->runGraphOp<nts::op::ForwardCPUfuseOp>(partitioned_graph,active,X[0]);
    delete partitioned_graph;  
    
    

    shuffle_vec(train_nids);
    // shuffle_vec(val_nids);
    // shuffle_vec(test_nids);
    // Sampler* train_sampler = new Sampler(fully_rep_graph, train_nids,graph->gnnctx->layer_size.size()-1,graph->config->batch_size,graph->gnnctx->fanout);
    // Sampler* eval_sampler = new Sampler(fully_rep_graph, val_nids,graph->gnnctx->layer_size.size()-1,graph->config->batch_size,graph->gnnctx->fanout);
    // Sampler* test_sampler = new Sampler(fully_rep_graph, test_nids,graph->gnnctx->layer_size.size()-1,graph->config->batch_size,graph->gnnctx->fanout);
    // cuda_stream = new Cuda_Stream();  
    cuda_stream = new Cuda_Stream[pipeline_num];
    auto default_stream = at::cuda::getDefaultCUDAStream();
    for(int i = 0; i < pipeline_num; i++) {
      torch_stream.push_back(at::cuda::getStreamFromPool(true));
      auto stream = torch_stream[i].stream();
      cuda_stream[i].setNewStream(stream);
      for(int j = 0; j < i; j++) {
        if(cuda_stream[j].stream == stream|| stream == default_stream) {
          std::printf("stream i:%p is repeat with j: %p, default: %p\n", stream, cuda_stream[j].stream, default_stream);
          exit(3);
        }
      }
    }


    int layer = graph->gnnctx->layer_size.size()-1;
    if(graph->config->pushdown)
        layer--;
    FastSampler* train_sampler = new FastSampler(graph,fully_rep_graph, 
        train_nids,layer,
            graph->gnnctx->fanout,graph->config->batch_size,true, 0, pipeline_num, cuda_stream);
//    train_sampler->set_ssg_num(pipeline_num, cuda_stream);

    FastSampler* eval_sampler = new FastSampler(graph,fully_rep_graph, 
        val_nids,layer,
            graph->gnnctx->fanout,graph->config->batch_size,true);

    FastSampler* test_sampler = new FastSampler(graph,fully_rep_graph,
        test_nids,layer,
            graph->gnnctx->fanout,graph->config->batch_size,true);

    exec_time -= get_time();


    for (int i_i = 0; i_i < iterations; i_i++) {
      double per_epoch_time = 0.0;
      per_epoch_time -= get_time();
      graph->rtminfo->epoch = i_i;
      if (i_i != 0) {
        for (int i = 0; i < P.size(); i++) {
          P[i]->zero_grad();
        }
      }
      ctx->train();
      Forward(train_sampler, 0);
//        ForwardPipeline(train_sampler, 0);
    //   ctx->eval();
    //   Forward(eval_sampler, 1);
    //   Forward(test_sampler, 2);
      per_epoch_time += get_time();

      std::cout << "GNNmini::Running.Epoch[" << i_i << "]:Times["
                  << per_epoch_time << "(s)]:loss\t" << loss << std::endl;
    }

    exec_time += get_time();
    printf("#run_time=%lf(s)\n", exec_time);
    printf("#sample_time=%lf(s)\n", (sample_time - train_sampler->copy_gpu_time));
    printf("#total_sample_time= %lf (s)\n", sample_time);
    printf("#transfer_feature_time=%lf(s)\n", (transfer_feature_time + train_sampler->copy_gpu_time));
    printf("training_cpu_time:%lf(s)\n",training_cpu_time);
    printf("#training_time=%lf(s)\n", training_time);
    printf("#gather_feature_time=%lf(s)\n", gather_feature_time);

    delete active;
  }

  // void cachenode_select(){
  //     MPI_Datatype vid_t = get_mpi_data_type<VertexId>();
  //     ntsc->sample_num = graph->alloc_interleaved_vertex_array<VertexId>();
  //     for (VertexId v_i = 0; v_i < graph->vertices; v_i++) {
  //         ntsc->sample_num[v_i] = 0;
  //     }
  //     while(sampler_all->sample_not_finished()){
  //           sampler_all->reservoir_sample_all(graph->gnnctx->layer_size.size()-1,
  //                                     graph->config->batch_size,
  //                                     graph->gnnctx->fanout,
  //                                     ntsc->cacheflag);
  //     }
      
  //     SampledSubgraph *sg;
  //     while(sampler_all->has_rest()){
  //       sg=sampler_all->get_one();
  //       for(auto id:sg->sampled_sgs[graph->gnnctx->layer_size.size()-2]->dst()){
  //           ntsc->sample_num[id]++;
  //       }
  //     }
  //     MPI_Allreduce(MPI_IN_PLACE, ntsc->sample_num, graph->vertices, vid_t, MPI_SUM,
  //                 MPI_COMM_WORLD);
  //     sampler_all->useagain();
  //     for (VertexId v_i = 0; v_i < graph->vertices; v_i++) {
  //         ntsc->sample_sort[v_i] = ntsc->sample_num[v_i];
  //     }
  //     sort(ntsc->sample_sort.begin(), ntsc->sample_sort.end());
  //     ntsc->cacheSamplenum = ntsc->sample_sort[ntsc->cacheBoundary];
  //     ntsc->fanoutBoundary = ntsc->fanoutRatio * graph->gnnctx->fanout[0];

  //     for(int idx = 0; idx < graph->vertices; idx++){
  //         VertexId nbrs = fully_rep_graph->column_offset[idx+1] - fully_rep_graph->column_offset[idx];
  //         bool cacheflag = (nbrs > ntsc->fanoutBoundary) ? false : true;
  //         if(ntsc->sample_num[idx] > ntsc->cacheSamplenum && cacheflag){
  //             ntsc->partition_cache[graph->get_partition_id(idx)].push_back(idx);
  //             ntsc->cacheflag[idx] = 1;
  //             ntsc->cachenum++;
  //           }
  //     }
  //     printf("cachenum:%d,vertexs:%d",ntsc->cachenum,graph->vertices);
  //     for(int i = 0; i < ntsc->partition_cache[graph->partition_id].size();i++){
  //         VertexId idx = ntsc->partition_cache[graph->partition_id][i];
  //         ntsc->map_cache.insert(std::make_pair(idx, i));
  //     }

  //     sampler_all->clear_queue();
  //     sampler_all->restart();
  // }

};

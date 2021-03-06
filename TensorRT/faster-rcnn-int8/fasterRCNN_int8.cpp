#include <cassert>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <sys/stat.h>
#include <cmath>
#include <time.h>
#include <cuda_runtime_api.h>
#include <cudnn.h>
#include <cublas_v2.h>
#include <memory>
#include <cstring>
#include <algorithm>
#include <iterator>

#include "NvCaffeParser.h"
#include "NvInferPlugin.h"
#include "common.h"
#include <cstdio>
#include <ctime>

#include "data_loader.h"

static Logger gLogger;
using namespace nvinfer1;
using namespace nvcaffeparser1;
using namespace plugin;

static const int INPUT_C = 3;
static const int INPUT_H = 375;
static const int INPUT_W = 500;
static const int IM_INFO_SIZE = 3;
static const int OUTPUT_CLS_SIZE = 21;
static const int OUTPUT_BBOX_SIZE = OUTPUT_CLS_SIZE * 4;

const std::string CLASSES[OUTPUT_CLS_SIZE]{ "background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus", "car", "cat", "chair", "cow", "diningtable", "dog", "horse", "motorbike", "person", "pottedplant", "sheep", "sofa", "train", "tvmonitor" };

const char* INPUT_BLOB_NAME0 = "data";
const char* INPUT_BLOB_NAME1 = "im_info";
const char* OUTPUT_BLOB_NAME0 = "bbox_pred";
const char* OUTPUT_BLOB_NAME1 = "cls_prob";
const char* OUTPUT_BLOB_NAME2 = "rois";

const int poolingH = 7;
const int poolingW = 7;
const int featureStride = 16;
const int preNmsTop = 6000;
const int nmsMaxOut = 300;
const int anchorsRatioCount = 3;
const int anchorsScaleCount = 3;
const float iouThreshold = 0.7f;
const float minBoxSize = 16;
const float spatialScale = 0.0625f;
const float anchorsRatios[anchorsRatioCount] = { 0.5f, 1.0f, 2.0f };
const float anchorsScales[anchorsScaleCount] = { 8.0f, 16.0f, 32.0f };

struct PPM
{
    std::string magic, fileName;
    int h, w, max;
    uint8_t buffer[INPUT_C*INPUT_H*INPUT_W];
};

std::string locateFile(const std::string& input)
{
    std::vector<std::string> dirs{"data/faster-rcnn/", "data/samples/faster-rcnn/"};
    return locateFile(input, dirs);
};

class Int8EntropyCalibrator : public IInt8EntropyCalibrator
{
public:
    Int8EntropyCalibrator(DataLoader* dataloader, int batch, int height, int width, int channel, bool readCache = true) : mReadCache(readCache)
    {
	_dataloader = dataloader;
	DimsNCHW dims = DimsNCHW(batch, channel, height, width);
	mInputCount1 = batch * dims.c() * dims.h() * dims.w();
	CHECK(cudaMalloc(&mDeviceInput1, mInputCount1 * sizeof(float)));
	mInputCount2 = batch * 3;
	CHECK(cudaMalloc(&mDeviceInput2, mInputCount2 * sizeof(float)));
    }
    virtual ~Int8EntropyCalibrator()
    {
	CHECK(cudaFree(mDeviceInput1));
	CHECK(cudaFree(mDeviceInput2));
    }
    int getBatchSize() const override { return 2; }
    bool getBatch(void* bindings[], const char* names[], int nbBindings) override
    {
	if(!_dataloader->next())
	    return false;
        CHECK(cudaMemcpy(mDeviceInput1, _dataloader->getBatch(),  mInputCount1 * sizeof(float), cudaMemcpyHostToDevice));
	CHECK(cudaMemcpy(mDeviceInput2, _dataloader->getIminfo(), mInputCount2 * sizeof(float), cudaMemcpyHostToDevice));
        bindings[0] = mDeviceInput1;
        bindings[1] = mDeviceInput2;
        return true;
    }
    const void* readCalibrationCache(size_t& length) override
    {
	std::cout << "Reading from cache: "<< calibrationTableName()<<std::endl;
	mCalibrationCache.clear();
	std::ifstream input(calibrationTableName(), std::ios::binary);
	input >> std::noskipws;
	if (mReadCache && input.good())
	    std::copy(std::istream_iterator<char>(input), std::istream_iterator<char>(), std::back_inserter(mCalibrationCache));
	length = mCalibrationCache.size();
	return length ? &mCalibrationCache[0] : nullptr;
    }
    void writeCalibrationCache(const void* cache, size_t length) override
    {
	std::ofstream output(calibrationTableName(), std::ios::binary);
	output.write(reinterpret_cast<const char*>(cache), length);
    }
private:
    static std::string calibrationTableName()
    {
        return std::string("CalibrationTable") + "vgg16";
    }
    bool mReadCache{ true };
    size_t mInputCount1;
    size_t mInputCount2;
    void* mDeviceInput1{ nullptr };
    void* mDeviceInput2{ nullptr };
    std::vector<char> mCalibrationCache;
    DataLoader* _dataloader;
};

void readPPMFile(const std::string& filename, PPM& ppm)
{
    ppm.fileName = filename;
    std::ifstream infile(filename, std::ifstream::binary);
    infile >> ppm.magic >> ppm.w >> ppm.h >> ppm.max;
    infile.seekg(1, infile.cur);
    infile.read(reinterpret_cast<char*>(ppm.buffer), ppm.w * ppm.h * 3);
}

void caffeToTRTModel(const std::string& deployFile, const std::string& modelFile, const std::vector<std::string>& outputs, unsigned int maxBatchSize, nvcaffeparser1::IPluginFactory* pluginFactory, IHostMemory **modelStream, DataType dataType)
{
    IBuilder* builder = createInferBuilder(gLogger);
    INetworkDefinition* network = builder->createNetwork();
    ICaffeParser* parser = createCaffeParser();
    parser->setPluginFactory(pluginFactory);
    std::cout << "Begin to parse model" <<std::endl;
    const IBlobNameToTensor* blobNameToTensor = parser->parse(deployFile.c_str(), modelFile.c_str(), *network, dataType == DataType::kINT8 ? DataType::kFLOAT : dataType);
    std::cout << "End to parse model" << std::endl;
    for (auto& s : outputs)
	network->markOutput(*blobNameToTensor->find(s.c_str()));
    builder->setMaxBatchSize(maxBatchSize);
    builder->setMaxWorkspaceSize(1 << 30);
    builder->setInt8Mode(true);
    DataLoader* dataLoader = new DataLoader(maxBatchSize, "/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/list.txt", 500, 375, 3);
    Int8EntropyCalibrator* calibrator = new Int8EntropyCalibrator(dataLoader, maxBatchSize, 375, 500, 3);
    builder->setInt8Calibrator(calibrator);
    std::cout << "Begin to build engine..." << std::endl;
    ICudaEngine* engine = builder->buildCudaEngine(*network);
    assert(engine);
    std::cout << "End to build engine..." << std::endl;
    network->destroy();
    parser->destroy();
    (*modelStream) = engine->serialize();
    engine->destroy();
    builder->destroy();
    shutdownProtobufLibrary();
}

float doInference(IExecutionContext& context, float* inputData, float* inputImInfo, float* outputBboxPred, float* outputClsProb, float *outputRois, int batchSize)
{
    std::cout << "Begin to do infer..." << std::endl;
    const ICudaEngine& engine = context.getEngine();
    assert(engine.getNbBindings() == 5);
    void* buffers[5];
    float ms = 0.0f;
    int inputIndex0 = engine.getBindingIndex(INPUT_BLOB_NAME0),
	inputIndex1 = engine.getBindingIndex(INPUT_BLOB_NAME1),
	outputIndex0 = engine.getBindingIndex(OUTPUT_BLOB_NAME0),
	outputIndex1 = engine.getBindingIndex(OUTPUT_BLOB_NAME1),
	outputIndex2 = engine.getBindingIndex(OUTPUT_BLOB_NAME2);
    CHECK(cudaMalloc(&buffers[inputIndex0], batchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(float)));   // data
    CHECK(cudaMalloc(&buffers[inputIndex1], batchSize * IM_INFO_SIZE * sizeof(float)));                  // im_info
    CHECK(cudaMalloc(&buffers[outputIndex0], batchSize * nmsMaxOut * OUTPUT_BBOX_SIZE * sizeof(float))); // bbox_pred
    CHECK(cudaMalloc(&buffers[outputIndex1], batchSize * nmsMaxOut * OUTPUT_CLS_SIZE * sizeof(float)));  // cls_prob
    CHECK(cudaMalloc(&buffers[outputIndex2], batchSize * nmsMaxOut * 4 * sizeof(float)));                // rois
    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));
    // DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
    CHECK(cudaMemcpyAsync(buffers[inputIndex0], inputData, batchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(float), cudaMemcpyHostToDevice, stream));
    CHECK(cudaMemcpyAsync(buffers[inputIndex1], inputImInfo, batchSize * IM_INFO_SIZE * sizeof(float), cudaMemcpyHostToDevice, stream));
    cudaStreamSynchronize(stream);
    double start = std::clock();
    int iter = 1;
    for(int i=0; i<iter; ++i)
    {
        context.enqueue(batchSize, buffers, stream, nullptr);
        cudaStreamSynchronize(stream);
    }
    ms = (std::clock()-start) / (double) CLOCKS_PER_SEC /iter * 1000;
    std::cout<< "infer total time elapse:  "<< ms << " ms" <<std::endl;
    CHECK(cudaMemcpyAsync(outputBboxPred, buffers[outputIndex0], batchSize * nmsMaxOut * OUTPUT_BBOX_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(outputClsProb, buffers[outputIndex1], batchSize * nmsMaxOut * OUTPUT_CLS_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(outputRois, buffers[outputIndex2], batchSize * nmsMaxOut * 4 * sizeof(float), cudaMemcpyDeviceToHost, stream));
    cudaStreamSynchronize(stream);
    CHECK(cudaFree(buffers[inputIndex0]));
    CHECK(cudaFree(buffers[inputIndex1]));
    CHECK(cudaFree(buffers[outputIndex0]));
    CHECK(cudaFree(buffers[outputIndex1]));
    CHECK(cudaFree(buffers[outputIndex2]));
    cudaStreamDestroy(stream);
    std::cout << "End to do infer..." << std::endl;
    return ms;
}

template<int OutC>
class Reshape : public IPlugin
{
public:
    Reshape() {}
    Reshape(const void* buffer, size_t size)
    {
	assert(size == sizeof(mCopySize));
	mCopySize = *reinterpret_cast<const size_t*>(buffer);
    }
    int getNbOutputs() const override {return 1;}
    Dims getOutputDimensions(int index, const Dims* inputs, int nbInputDims) override
    {
	assert(nbInputDims == 1);
	assert(index == 0);
	assert(inputs[index].nbDims == 3);
	assert((inputs[0].d[0])*(inputs[0].d[1]) % OutC == 0);
	return DimsCHW(OutC, inputs[0].d[0] * inputs[0].d[1] / OutC, inputs[0].d[2]);
    }
    int initialize() override {return 0;}
    void terminate() override {}
    size_t getWorkspaceSize(int) const override {return 0;}
    // currently it is not possible for a plugin to execute "in place". Therefore we memcpy the data from the input to the output buffer
    int enqueue(int batchSize, const void*const *inputs, void** outputs, void*, cudaStream_t stream) override
    {
	CHECK(cudaMemcpyAsync(outputs[0], inputs[0], mCopySize * batchSize, cudaMemcpyDeviceToDevice, stream));
	return 0;
    }
    size_t getSerializationSize() override {return sizeof(mCopySize);}
    void serialize(void* buffer) override {*reinterpret_cast<size_t*>(buffer) = mCopySize;}
    void configure(const Dims*inputs, int nbInputs, const Dims* outputs, int nbOutputs, int) override
    {
	mCopySize = inputs[0].d[0] * inputs[0].d[1] * inputs[0].d[2] * sizeof(float);
    }
protected:
    size_t mCopySize;
};

// integration for serialization
class PluginFactory : public nvinfer1::IPluginFactory, public nvcaffeparser1::IPluginFactory
{
public:
    // deserialization plugin implementation
    virtual nvinfer1::IPlugin* createPlugin(const char* layerName, const nvinfer1::Weights* weights, int nbWeights) override
    {
	assert(isPlugin(layerName));
	if (!strcmp(layerName, "ReshapeCTo2"))
	{
	    assert(mPluginRshp2 == nullptr);
	    assert(nbWeights == 0 && weights == nullptr);
	    mPluginRshp2 = std::unique_ptr<Reshape<2>>(new Reshape<2>());
	    return mPluginRshp2.get();
	}
	else if (!strcmp(layerName, "ReshapeCTo18"))
	{
	    assert(mPluginRshp18 == nullptr);
	    assert(nbWeights == 0 && weights == nullptr);
	    mPluginRshp18 = std::unique_ptr<Reshape<18>>(new Reshape<18>());
	    return mPluginRshp18.get();
	}
	else if (!strcmp(layerName, "RPROIFused"))
	{
	    assert(mPluginRPROI == nullptr);
	    assert(nbWeights == 0 && weights == nullptr);
	    mPluginRPROI = std::unique_ptr<INvPlugin, decltype(nvPluginDeleter)>
		(createFasterRCNNPlugin(featureStride, preNmsTop, nmsMaxOut, iouThreshold, minBoxSize, spatialScale,
		    DimsHW(poolingH, poolingW), Weights{ nvinfer1::DataType::kFLOAT, anchorsRatios, anchorsRatioCount },
		    Weights{ nvinfer1::DataType::kFLOAT, anchorsScales, anchorsScaleCount }), nvPluginDeleter);
	    return mPluginRPROI.get();
	}
	else
	{
	    assert(0);
	    return nullptr;
	}
    }
    IPlugin* createPlugin(const char* layerName, const void* serialData, size_t serialLength) override
    {
	assert(isPlugin(layerName));
	if (!strcmp(layerName, "ReshapeCTo2"))
	{
	    assert(mPluginRshp2 == nullptr);
	    mPluginRshp2 = std::unique_ptr<Reshape<2>>(new Reshape<2>(serialData, serialLength));
	    return mPluginRshp2.get();
	}
	else if (!strcmp(layerName, "ReshapeCTo18"))
	{
	    assert(mPluginRshp18 == nullptr);
	    mPluginRshp18 = std::unique_ptr<Reshape<18>>(new Reshape<18>(serialData, serialLength));
	    return mPluginRshp18.get();
	}
	else if (!strcmp(layerName, "RPROIFused"))
	{
	    assert(mPluginRPROI == nullptr);
	    mPluginRPROI = std::unique_ptr<INvPlugin, decltype(nvPluginDeleter)>
	        (createFasterRCNNPlugin(serialData, serialLength), nvPluginDeleter);
	    return mPluginRPROI.get();
	}
	else
	{
	    assert(0);
	    return nullptr;
	}
    }
    // caffe parser plugin implementation
    bool isPlugin(const char* name) override
    {
	return (!strcmp(name, "ReshapeCTo2") || !strcmp(name, "ReshapeCTo18") || !strcmp(name, "RPROIFused"));
    }
    // the application has to destroy the plugin when it knows it's safe to do so
    void destroyPlugin()
    {
	mPluginRshp2.release();
	mPluginRshp2 = nullptr;
	mPluginRshp18.release();
	mPluginRshp18 = nullptr;
	mPluginRPROI.release();
	mPluginRPROI = nullptr;
    }
    std::unique_ptr<Reshape<2>> mPluginRshp2{ nullptr };
    std::unique_ptr<Reshape<18>> mPluginRshp18{ nullptr };
    void(*nvPluginDeleter)(INvPlugin*) { [](INvPlugin* ptr) {ptr->destroy(); } };
    std::unique_ptr<INvPlugin, decltype(nvPluginDeleter)> mPluginRPROI{ nullptr, nvPluginDeleter };
};

void bboxTransformInvAndClip(float* rois, float* deltas, float* predBBoxes, float* imInfo, const int N, const int nmsMaxOut, const int numCls)
{
    float width, height, ctr_x, ctr_y;
    float dx, dy, dw, dh, pred_ctr_x, pred_ctr_y, pred_w, pred_h;
    float *deltas_offset, *predBBoxes_offset, *imInfo_offset;
    for (int i = 0; i < N * nmsMaxOut; ++i)
    {
	width = rois[i * 4 + 2] - rois[i * 4] + 1;
	height = rois[i * 4 + 3] - rois[i * 4 + 1] + 1;
	ctr_x = rois[i * 4] + 0.5f * width;
	ctr_y = rois[i * 4 + 1] + 0.5f * height;
	deltas_offset = deltas + i * numCls * 4;
	predBBoxes_offset = predBBoxes + i * numCls * 4;
	imInfo_offset = imInfo + i / nmsMaxOut * 3;
	for (int j = 0; j < numCls; ++j)
	{
	    dx = deltas_offset[j * 4];
	    dy = deltas_offset[j * 4 + 1];
	    dw = deltas_offset[j * 4 + 2];
	    dh = deltas_offset[j * 4 + 3];
	    pred_ctr_x = dx * width + ctr_x;
	    pred_ctr_y = dy * height + ctr_y;
	    pred_w = exp(dw) * width;
	    pred_h = exp(dh) * height;
	    predBBoxes_offset[j * 4] = std::max(std::min(pred_ctr_x - 0.5f * pred_w, imInfo_offset[1] - 1.f), 0.f);
	    predBBoxes_offset[j * 4 + 1] = std::max(std::min(pred_ctr_y - 0.5f * pred_h, imInfo_offset[0] - 1.f), 0.f);
	    predBBoxes_offset[j * 4 + 2] = std::max(std::min(pred_ctr_x + 0.5f * pred_w, imInfo_offset[1] - 1.f), 0.f);
	    predBBoxes_offset[j * 4 + 3] = std::max(std::min(pred_ctr_y + 0.5f * pred_h, imInfo_offset[0] - 1.f), 0.f);
	}
    }
}

std::vector<int> nms(std::vector<std::pair<float, int> >& score_index, float* bbox, const int classNum, const int numClasses, const float nms_threshold)
{
    auto overlap1D = [](float x1min, float x1max, float x2min, float x2max) -> float 
    {
	if (x1min > x2min)
	{
	    std::swap(x1min, x2min);
	    std::swap(x1max, x2max);
	}
	return x1max < x2min ? 0 : std::min(x1max, x2max) - x2min;
    };
    auto computeIoU = [&overlap1D](float* bbox1, float* bbox2) -> float 
    {
	float overlapX = overlap1D(bbox1[0], bbox1[2], bbox2[0], bbox2[2]);
	float overlapY = overlap1D(bbox1[1], bbox1[3], bbox2[1], bbox2[3]);
	float area1 = (bbox1[2] - bbox1[0]) * (bbox1[3] - bbox1[1]);
	float area2 = (bbox2[2] - bbox2[0]) * (bbox2[3] - bbox2[1]);
	float overlap2D = overlapX * overlapY;
	float u = area1 + area2 - overlap2D;
	return u == 0 ? 0 : overlap2D / u;
    };
    std::vector<int> indices;
    for (auto i : score_index)
    {
	const int idx = i.second;
	bool keep = true;
	for (unsigned k = 0; k < indices.size(); ++k)
	{
	    if (keep)
	    {
		const int kept_idx = indices[k];
		float overlap = computeIoU(&bbox[(idx*numClasses + classNum) * 4], &bbox[(kept_idx*numClasses + classNum) * 4]);
		keep = overlap <= nms_threshold;
	    }
	    else
		break;
	}
	if (keep) indices.push_back(idx);
    }
    return indices;
}

int main(int argc, char* argv[])
{
    PluginFactory pluginFactory;
    IHostMemory *modelStream{ nullptr };
    const int N = 5;
    caffeToTRTModel("/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/faster_rcnn_test_iplugin.prototxt", 
		    "/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/VGG16_faster_rcnn_final.caffemodel",
		    std::vector < std::string > { OUTPUT_BLOB_NAME0, OUTPUT_BLOB_NAME1, OUTPUT_BLOB_NAME2 },
		    N, &pluginFactory, &modelStream, DataType::kINT8);
    pluginFactory.destroyPlugin();
    std::vector<std::string> imageList = { "/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/1.ppm",
					  "/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/2.ppm",
					  "/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/3.ppm",
					  "/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/4.ppm",
					  "/home/cd/TensorRT-4.0.1.6/data/faster-rcnn/5.ppm" };
    std::vector<PPM> ppms(N);
    float imInfo[N * 3];
    assert(ppms.size() <= imageList.size());
    for (int i = 0; i < N; ++i)
    {
	readPPMFile(imageList[i], ppms[i]);
	imInfo[i * 3] = float(ppms[i].h);   // number of rows
	imInfo[i * 3 + 1] = float(ppms[i].w); // number of columns
	imInfo[i * 3 + 2] = 1;         // image scale
    }
    float* data = new float[N*INPUT_C*INPUT_H*INPUT_W];
    // pixel mean used by the Faster R-CNN's author
    float pixelMean[3]{ 102.9801f, 115.9465f, 122.7717f }; // also in BGR order
    for (int i = 0, volImg = INPUT_C*INPUT_H*INPUT_W; i < N; ++i)
    {
	for (int c = 0; c < INPUT_C; ++c)
	{
	    // the color image to input should be in BGR order
	    for (unsigned j = 0, volChl = INPUT_H*INPUT_W; j < volChl; ++j)
		data[i*volImg + c*volChl + j] = float(ppms[i].buffer[j*INPUT_C + 2 - c]) - pixelMean[c];
	}
    }
    IRuntime* runtime = createInferRuntime(gLogger);
    ICudaEngine* engine = runtime->deserializeCudaEngine(modelStream->data(), modelStream->size(), &pluginFactory);
    IExecutionContext *context = engine->createExecutionContext();
    float* rois = new float[N * nmsMaxOut * 4];
    float* bboxPreds = new float[N * nmsMaxOut * OUTPUT_BBOX_SIZE];
    float* clsProbs = new float[N * nmsMaxOut * OUTPUT_CLS_SIZE];
    float* predBBoxes = new float[N * nmsMaxOut * OUTPUT_BBOX_SIZE];
    float totalTime = 0.0f;
    totalTime = doInference(*context, data, imInfo, bboxPreds, clsProbs, rois, N);
    std::cout << "average infer time of each image is: " << totalTime / N << " ms" << std::endl;
    context->destroy();
    engine->destroy();
    runtime->destroy();
    pluginFactory.destroyPlugin();
    for (int i = 0; i < N; ++i)
    {
	float * rois_offset = rois + i * nmsMaxOut * 4;
	for (int j = 0; j < nmsMaxOut * 4 && imInfo[i * 3 + 2] != 1; ++j)
	    rois_offset[j] /= imInfo[i * 3 + 2];
    }
    bboxTransformInvAndClip(rois, bboxPreds, predBBoxes, imInfo, N, nmsMaxOut, OUTPUT_CLS_SIZE);
    const float nms_threshold = 0.3f;
    const float score_threshold = 0.8f;
    for (int i = 0; i < N; ++i)
    {
	float *bbox = predBBoxes + i * nmsMaxOut * OUTPUT_BBOX_SIZE;
	float *scores = clsProbs + i * nmsMaxOut * OUTPUT_CLS_SIZE;
	for (int c = 1; c < OUTPUT_CLS_SIZE; ++c) // skip the background
	{
	    std::vector<std::pair<float, int> > score_index;
	    for (int r = 0; r < nmsMaxOut; ++r)
	    {
		if (scores[r*OUTPUT_CLS_SIZE + c] > score_threshold)
		{
		    score_index.push_back(std::make_pair(scores[r*OUTPUT_CLS_SIZE + c], r));
		    std::stable_sort(score_index.begin(), score_index.end(), [](const std::pair<float, int>& pair1, const std::pair<float, int>& pair2) {return pair1.first > pair2.first;});
		}
	    }
	    std::vector<int> indices = nms(score_index, bbox, c, OUTPUT_CLS_SIZE, nms_threshold);
	    for (unsigned k = 0; k < indices.size(); ++k)
	    {
		int idx = indices[k];
		std::cout << "Detected " << CLASSES[c] << " in " << ppms[i].fileName << " with confidence " << scores[idx*OUTPUT_CLS_SIZE + c] * 100.0f << "% " << std::endl;
	    }
	}
    }
    delete[] data;
    delete[] rois;
    delete[] bboxPreds;
    delete[] clsProbs;
    delete[] predBBoxes;
    return 0;
}

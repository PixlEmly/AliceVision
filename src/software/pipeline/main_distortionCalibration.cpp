/*Command line parameters*/
#include <Eigen/Dense>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <sstream>

#include <aliceVision/system/cmdline.hpp>
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/system/main.hpp>
#include <aliceVision/system/Timer.hpp>

#include <aliceVision/sfmData/SfMData.hpp>
#include <aliceVision/sfmDataIO/sfmDataIO.hpp>

#include <aliceVision/image/all.hpp>
#include <OpenImageIO/imagebufalgo.h>

#include <opencv2/opencv.hpp>

#include <aliceVision/calibration/distortionEstimation.hpp>
#include <aliceVision/calibration/checkerDetector.hpp>

#include "libcbdetect/boards_from_corners.h"
#include "libcbdetect/config.h"
#include "libcbdetect/find_corners.h"
#include "libcbdetect/plot_boards.h"
#include "libcbdetect/plot_corners.h"
#include <chrono>
#include <opencv2/opencv.hpp>
#include <vector>



// These constants define the current software version.
// They must be updated when the command line is changed.
#define ALICEVISION_SOFTWARE_VERSION_MAJOR 0
#define ALICEVISION_SOFTWARE_VERSION_MINOR 1

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace aliceVision;



image::Image<image::RGBColor> undistort(Vec2 & offset, const std::shared_ptr<camera::Pinhole> & camera, const image::Image<image::RGBColor> & source) 
{
    double w = source.Width();
    double h = source.Height();

    double minx = w;
    double maxx = 0;
    double miny = h;
    double maxy = 0;

    for (int i = 0; i < h; i++) 
    {
        for (int j = 0; j < w; j++)
        {
            Vec2 pos = camera->get_ud_pixel(Vec2(j, i));
            minx = std::min(minx, pos.x());
            maxx = std::max(maxx, pos.x());
            miny = std::min(miny, pos.y());
            maxy = std::max(maxy, pos.y());
        }
    }

    int width = maxx - minx + 1;
    int height = maxy - miny + 1;

    image::Image<image::RGBColor> result(width, height, true, image::RGBColor(0, 0, 0));

    const image::Sampler2d<image::SamplerLinear> sampler;

    for (int i = 0; i < height; i++) 
    {
        double y = miny + double(i);

        for (int j = 0; j < width; j++)
        {
            double x = minx + double(j);

            Vec2 pos(x, y);
            Vec2 dist = camera->get_d_pixel(pos);


            if (dist.x() < 0 || dist.x() >= source.Width()) continue;
            if (dist.y() < 0 || dist.y() >= source.Height()) continue;
            
            image::RGBColor c = sampler(source, dist.y(), dist.x());
            
            result(i, j) = c;
        }
    }

    offset.x() = minx;
    offset.y() = miny;

    return result;
}

image::Image<image::RGBfColor> undistortSTMAP(Vec2 & offset, const std::shared_ptr<camera::Pinhole> & camera, const image::Image<image::RGBColor> & source) 
{
    double w = source.Width();
    double h = source.Height();

    double minx = w;
    double maxx = 0;
    double miny = h;
    double maxy = 0;

    for (int i = 0; i < h; i++) 
    {
        for (int j = 0; j < w; j++)
        {
            Vec2 pos = camera->get_ud_pixel(Vec2(j, i));
            minx = std::min(minx, pos.x());
            maxx = std::max(maxx, pos.x());
            miny = std::min(miny, pos.y());
            maxy = std::max(maxy, pos.y());
        }
    }

    int width = maxx - minx + 1;
    int height = maxy - miny + 1;

    image::Image<image::RGBfColor> result(width, height, true, image::FBLACK);

    const image::Sampler2d<image::SamplerLinear> sampler;

    for (int i = 0; i < height; i++) 
    {
        double y = miny + double(i);

        for (int j = 0; j < width; j++)
        {
            double x = minx + double(j);

            Vec2 pos(x, y);
            Vec2 dist = camera->get_d_pixel(pos);


            if (dist.x() < 0 || dist.x() >= source.Width()) continue;
            if (dist.y() < 0 || dist.y() >= source.Height()) continue;
            
            
            result(i, j).r() = dist.x() / (float(width) - 1);
            result(i, j).g() = (float(height) - 1.0f - dist.y()) / (float(height) - 1.0f);
            result(i, j).b() = 0.0f;
        }
    }

    offset.x() = minx;
    offset.y() = miny;

    return result;
}

bool retrieveLines(std::vector<calibration::LineWithPoints> & lineWithPoints, const image::Image<image::RGBColor> & input, const std::string & checkerImagePath)
{
    cv::Mat inputOpencvWrapper(input.Height(), input.Width(), CV_8UC3, (void*)input.data());

    cbdetect::Params params;
    params.show_processing = false;
    params.show_debug_image = false;
    params.corner_type = cbdetect::SaddlePoint;
    params.norm = true;

    cbdetect::Corner corners;
    cbdetect::find_corners(inputOpencvWrapper, corners, params);
    if (corners.p.size() < 20) 
    {
        return false;
    }

    double hw = double(input.Width()) / 2.0;
    double hh = double(input.Height()) / 2.0;

    cbdetect::Corner filtered_corners;
    for (int id = 0; id < corners.p.size(); id++)
    {
        double x = std::abs(corners.p[id].x - hw);
        double y = std::abs(corners.p[id].y - hh);

        if (x < 100 && y < 100) 
        {
            continue;
        }

        filtered_corners.p.push_back(corners.p[id]);
        filtered_corners.v1.push_back(corners.v1[id]);
        filtered_corners.v2.push_back(corners.v2[id]);
        filtered_corners.r.push_back(corners.r[id]);
        filtered_corners.score.push_back(corners.score[id]);
    }

    std::vector<cbdetect::Board> boards;
    int iteration = 0;
    while (iteration < 10)
    {
        boards.clear();
        cbdetect::boards_from_corners(inputOpencvWrapper, filtered_corners, boards, params);
        
        int hasWeirdAngle = 0;
        for (cbdetect::Board & b : boards)
        {
            int height = b.idx.size();
            int width = b.idx[0].size();

            for (int i = 0; i < height - 1; i ++)
            {
                for (int j = 0; j < width - 1; j++)
                {
                    int idx = b.idx[i][j];
                    if (idx < 0) continue;

                    cv::Point2d p = filtered_corners.p[idx];

                    int idxX = b.idx[i][j + 1];
                    if (idxX < 0) 
                    {
                        continue;
                    }

                    int idxY = b.idx[i + 1][j];
                    if (idxY < 0) 
                    {
                        continue;
                    }

                    cv::Point2d px = filtered_corners.p[idxX];
                    cv::Point2d py = filtered_corners.p[idxY];

                    px.x = px.x - p.x;
                    px.y = px.y - p.y;
                    py.x = py.x - p.x;
                    py.y = py.y - p.y;

                    double normx = sqrt(px.x * px.x + px.y * px.y);
                    if (normx < 1e-6) 
                    {
                        hasWeirdAngle++;
                        continue;
                    }

                    double normy = sqrt(py.x * py.x + py.y * py.y);
                    if (normy < 1e-6) 
                    {
                        hasWeirdAngle++;
                        continue;
                    }

                    double ca = (px / normx).dot(py / normy);
                    double angle = std::abs(std::acos(ca) - M_PI_2);
                    if (angle > M_PI_4)
                    {
                        hasWeirdAngle++;
                    }
                }   
            }
        }

        std::cout << hasWeirdAngle << "!!!"<< std::endl;

        if (hasWeirdAngle == 0)
        {
            break;
        }

        iteration++;
    }

    
    

    /*cv::Mat draw = inputOpencvWrapper.clone();
    for (cbdetect::Board & b : boards)
    {
        int height = b.idx.size();
        int width = b.idx[0].size();

        // Create horizontal lines
        for (int i = 0; i < height - 1; i ++)
        {
            for (int j = 0; j < width - 1; j++)
            {
                int idx = b.idx[i][j];
                if (idx < 0) continue;

                cv::Point2d p = filtered_corners.p[idx];

                cv::circle(draw, p, 5, cv::Scalar(255,0,0));

                idx = b.idx[i][j + 1];
                if (idx >=0) 
                {
                    cv::Point2d p2 = filtered_corners.p[idx];
                    cv::line(draw, p, p2, cv::Scalar(0,0,255));    
                }

                idx = b.idx[i + 1][j];
                if (idx >=0) 
                {
                    cv::Point2d p2 = filtered_corners.p[idx];
                    cv::line(draw, p, p2, cv::Scalar(0,0,255));    
                }
            }
        }
    }

    cv::imwrite(checkerImagePath, draw);*/
    
    
    lineWithPoints.clear();
    for (cbdetect::Board & b : boards)
    {
        int height = b.idx.size();
        int width = b.idx[0].size();

        // Create horizontal lines
        for (int i = 0; i < height; i ++)
        {
            //Random init
            calibration::LineWithPoints line;
            line.angle = M_PI_4;
            line.dist = 1;
            line.horizontal = true;
            line.index = i;

            for (int j = 0; j < width; j++)
            {
                int idx = b.idx[i][j];
                if (idx < 0) continue;

                cv::Point2d p = filtered_corners.p[idx];

                Vec2 pt;
                pt.x() = p.x;
                pt.y() = p.y;

                line.points.push_back(pt);
            }

            //Check we don't have a too small line which won't be easy to estimate
            if (line.points.size() < 10) continue;
            lineWithPoints.push_back(line);
        }

        // Create vertical lines
        for (int j = 0; j < width; j++)
        {
            calibration::LineWithPoints line;
            line.angle = M_PI_4;
            line.dist = 1;
            line.horizontal = false;
            line.index = j;

            for (int i = 0; i < height; i++)
            {
                int idx = b.idx[i][j];
                if (idx < 0) continue;

                cv::Point2d p = filtered_corners.p[idx];

                Vec2 pt;
                pt.x() = p.x;
                pt.y() = p.y;

                line.points.push_back(pt);
            }

            //Check we don't have a too small line which won't be easy to estimate
            if (line.points.size() < 10) continue;

            lineWithPoints.push_back(line);
        }
    }

    return true;
}

template <class T>
bool estimateDistortionK1(std::shared_ptr<camera::Pinhole> & camera, calibration::Statistics & statistics, std::vector<T> & items)
{
    std::vector<bool> locksDistortions = {true};

    //Everything locked except lines paramters
    locksDistortions[0] = true;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax distortion 1st order
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax offcenter
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    return true;
}

template <class T>
bool estimateDistortionK3(std::shared_ptr<camera::Pinhole> & camera, calibration::Statistics & statistics, std::vector<T> & items)
{
    std::vector<bool> locksDistortions = {true, true, true};

    //Everything locked except lines paramters
    locksDistortions[0] = true;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax distortion 1st order
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax offcenter
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax offcenter
    locksDistortions[0] = false;
    locksDistortions[1] = false;
    locksDistortions[2] = false;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    return true;
}

template <class T>
bool estimateDistortion3DER4(std::shared_ptr<camera::Pinhole> & camera, calibration::Statistics & statistics, std::vector<T> & items)
{
    std::vector<bool> locksDistortions = {true, true, true, true, true, true};

    //Everything locked except lines paramters
    locksDistortions[0] = true;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax distortion 1st order
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax offcenter
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax offcenter
    locksDistortions[0] = false;
    locksDistortions[1] = false;
    locksDistortions[2] = false;
    locksDistortions[3] = false;
    locksDistortions[4] = false;
    locksDistortions[5] = false;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    return true;
}

template <class T>
bool estimateDistortion3DEA4(std::shared_ptr<camera::Pinhole> & camera, calibration::Statistics & statistics, std::vector<T> & items)
{
    
    
    std::shared_ptr<camera::Pinhole> simpleCamera = std::make_shared<camera::PinholeRadialK1>(camera->w(), camera->h(), camera->getScale()[0], camera->getScale()[1], camera->getOffset()[0], camera->getOffset()[1], 0.0);
    if (!estimateDistortionK1(simpleCamera, statistics, items))
    {
        return false;
    }

    std::vector<bool> locksDistortions = {true, true, true, true};

    //Relax distortion all orders
    locksDistortions[0] = false;
    locksDistortions[1] = false;
    locksDistortions[2] = false;
    locksDistortions[3] = false;

    double k1 = simpleCamera->getDistortionParams()[0];
    camera->setDistortionParams({k1,k1,k1,k1});

    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    return true;
}

template <class T>
bool estimateDistortion3DELD(std::shared_ptr<camera::Pinhole> & camera, calibration::Statistics & statistics, std::vector<T> & items)
{
    std::vector<double> params = camera->getDistortionParams();
    params[0] = 0.0;
    params[1] = M_PI_2;
    params[2] = 0.0;
    params[3] = 0.0;
    params[4] = 0.0;
    camera->setDistortionParams(params);

    std::vector<bool> locksDistortions = {true, true, true, true, true};
    

    //Everything locked except lines paramters
    locksDistortions[0] = true;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    //Relax distortion 1st order
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, true, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }
    std::cout << camera->getDistortionParams() << std::endl;

    //Relax offcenter
    locksDistortions[0] = false;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }
    std::cout << camera->getDistortionParams() << std::endl;

    //Relax offcenter
    locksDistortions[0] = false;
    locksDistortions[1] = true;
    locksDistortions[2] = false;
    locksDistortions[3] = false;
    locksDistortions[4] = true;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }
    std::cout << camera->getDistortionParams() << std::endl;

    //Relax offcenter
    locksDistortions[0] = false;
    locksDistortions[1] = false;
    locksDistortions[2] = false;
    locksDistortions[3] = false;
    locksDistortions[4] = false;
    if (!calibration::estimate(camera, statistics, items, true, false, locksDistortions))
    {
        ALICEVISION_LOG_ERROR("Failed to calibrate");
        return false;
    }

    std::cout << camera->getDistortionParams() << std::endl;

    return true;
}

bool generatePoints(std::vector<calibration::PointPair> & points, const std::shared_ptr<camera::Pinhole> & camera, const std::vector<calibration::LineWithPoints> & lineWithPoints)
{

    for (auto & l : lineWithPoints)
    {
        for (auto & pt : l.points)
        {
            calibration::PointPair pp;

            //Everything is reverted in the given model (distorting equals to undistorting)
            pp.undistortedPoint = camera->get_d_pixel(pt);
            pp.distortedPoint = pt;

            double err =  (camera->get_ud_pixel(pp.undistortedPoint) - pp.distortedPoint).norm();
            if (err > 1e-3)
            {
                continue;
            }

            points.push_back(pp);
        }
    }

    return true;
}

int aliceVision_main(int argc, char* argv[])
{
    std::string sfmInputDataFilepath;
    std::string sfmOutputDataFilepath;
    std::string verboseLevel = system::EVerboseLevel_enumToString(system::Logger::getDefaultVerboseLevel());

    // Command line parameters
    po::options_description allParams(
    "Parse external information about cameras used in a panorama.\n"
    "AliceVision PanoramaInit");

    po::options_description requiredParams("Required parameters");
    requiredParams.add_options()
    ("input,i", po::value<std::string>(&sfmInputDataFilepath)->required(), "SfMData file input.")
    ("outSfMData,o", po::value<std::string>(&sfmOutputDataFilepath)->required(), "SfMData file output.")
    ;

    po::options_description logParams("Log parameters");
    logParams.add_options()
    ("verboseLevel,v", po::value<std::string>(&verboseLevel)->default_value(verboseLevel),
        "verbosity level (fatal, error, warning, info, debug, trace).");

    allParams.add(requiredParams).add(logParams);

    // Parse command line
    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, allParams), vm);

        if(vm.count("help") || (argc == 1))
        {
            ALICEVISION_COUT(allParams);
            return EXIT_SUCCESS;
        }
        po::notify(vm);
    }
    catch(boost::program_options::required_option& e)
    {
        ALICEVISION_CERR("ERROR: " << e.what());
        ALICEVISION_COUT("Usage:\n\n" << allParams);
        return EXIT_FAILURE;
    }
    catch(boost::program_options::error& e)
    {
        ALICEVISION_CERR("ERROR: " << e.what());
        ALICEVISION_COUT("Usage:\n\n" << allParams);
        return EXIT_FAILURE;
    }

    ALICEVISION_COUT("Program called with the following parameters:");
    ALICEVISION_COUT(vm);

    system::Logger::get()->setLogLevel(verboseLevel);

    sfmData::SfMData sfmData;
    if(!sfmDataIO::Load(sfmData, sfmInputDataFilepath, sfmDataIO::ESfMData(sfmDataIO::ALL)))
    {
        ALICEVISION_LOG_ERROR("The input SfMData file '" << sfmInputDataFilepath << "' cannot be read.");
        return EXIT_FAILURE;
    }

    // Analyze path
    boost::filesystem::path path(sfmOutputDataFilepath);
    std::string outputPath = path.parent_path().string();

    int pos = 0;
    for (auto v : sfmData.getViews()) 
    {
        auto view = v.second;
        const std::string viewIdStr = std::to_string(v.first);

        ALICEVISION_LOG_INFO("Processing view " << v.first);

        //Check pixel ratio
        float pixelRatio = view->getDoubleMetadata({"PixelAspectRatio"});
        if (pixelRatio < 0.0) 
        {
            pixelRatio = 1.0f;
        }

        //Read image
        image::Image<image::RGBColor> input;
        std::string pathImage = view->getImagePath();
        image::readImage(v.second->getImagePath(), input, image::EImageColorSpace::SRGB);       

        if (pixelRatio != 1.0f)
        {
            double w = input.Width();
            double h = input.Height();
            double nw = w;
            double nh = h / pixelRatio;
            image::Image<image::RGBColor> resizedInput(nw, nh);

            const oiio::ImageSpec imageSpecResized(nw, nh, 3, oiio::TypeDesc::UCHAR);
            const oiio::ImageSpec imageSpecOrigin(w, h, 3, oiio::TypeDesc::UCHAR);

            const oiio::ImageBuf inBuf(imageSpecOrigin, input.data());
            oiio::ImageBuf outBuf(imageSpecResized, resizedInput.data());

            oiio::ImageBufAlgo::resize(outBuf, inBuf);
            input.swap(resizedInput);
        }

        
        std::shared_ptr<camera::IntrinsicBase> cameraBase = sfmData.getIntrinsicsharedPtr(v.second->getIntrinsicId());
        std::shared_ptr<camera::Pinhole> cameraPinhole = std::dynamic_pointer_cast<camera::Pinhole>(cameraBase);
        if (!cameraPinhole)
        {
            ALICEVISION_LOG_ERROR("Only work for pinhole cameras");
            continue;
        }

        Vec2 originalScale = cameraPinhole->getScale();
        
        double w = input.Width();
        double h = input.Height();
        double hw = w / 2.0;
        double hh = h / 2.0;
        double d = sqrt(hw*hw + hh*hh);

        //Force the 'focal' to normalize the image such that its semi diagonal is 1
        cameraPinhole->setWidth(w);
        cameraPinhole->setHeight(h);
        cameraPinhole->setScale(d, d);
        cameraPinhole->setOffset(hw, hh);
        
        fs::copy_file(view->getImagePath(), fs::path(outputPath) / fs::path(view->getImagePath()).filename(), fs::copy_option::overwrite_if_exists);

        std::string undistortedImagePath = (fs::path(outputPath) / fs::path(view->getImagePath()).stem()).string() + "_undistorted.exr";
        std::string stMapImagePath = (fs::path(outputPath) / fs::path(view->getImagePath()).stem()).string() + "_stmap.exr";
        std::string checkerImagePath = (fs::path(outputPath) / fs::path(view->getImagePath()).stem()).string() + "_checkerboard.exr";

        calibration::CheckerDetector detect;
        detect.process(input);

        //Retrieve lines
        std::vector<calibration::LineWithPoints> lineWithPoints;
        if (!retrieveLines(lineWithPoints, input, checkerImagePath))
        {
            ALICEVISION_LOG_ERROR("Impossible to extract the checkerboards lines");
            continue;
            
        }

        calibration::Statistics statistics;
 
        //Estimate distortion
        if (std::dynamic_pointer_cast<camera::PinholeRadialK1>(cameraBase))
        {
            if (!estimateDistortionK1(cameraPinhole, statistics, lineWithPoints))
            {
                ALICEVISION_LOG_ERROR("Error estimating distortion");
                continue;
            }
        }
        else if (std::dynamic_pointer_cast<camera::PinholeRadialK3>(cameraBase))
        {
            if (!estimateDistortionK3(cameraPinhole, statistics, lineWithPoints))
            {
                ALICEVISION_LOG_ERROR("Error estimating distortion");
                continue;
            }
        }
        
        else if (std::dynamic_pointer_cast<camera::Pinhole3DERadial4>(cameraBase))
        {
            if (!estimateDistortion3DER4(cameraPinhole, statistics, lineWithPoints))
            {
                ALICEVISION_LOG_ERROR("Error estimating distortion");
                continue;
            }
        }
        else if (std::dynamic_pointer_cast<camera::Pinhole3DEAnamorphic4>(cameraBase))
        {
            if (!estimateDistortion3DEA4(cameraPinhole, statistics, lineWithPoints))
            {
                ALICEVISION_LOG_ERROR("Error estimating distortion");
                continue;
            }
        }
        else if (std::dynamic_pointer_cast<camera::Pinhole3DEClassicLD>(cameraBase))
        {
            if (!estimateDistortion3DELD(cameraPinhole, statistics, lineWithPoints))
            {
                ALICEVISION_LOG_ERROR("Error estimating distortion");
                continue;
            }
        }
        else 
        {
            ALICEVISION_LOG_ERROR("Incompatible camera distortion model");
        }

        ALICEVISION_LOG_INFO("Result quality of calibration : ");
        ALICEVISION_LOG_INFO("Mean of error (stddev) : " << statistics.mean << "(" << statistics.stddev <<")");
        ALICEVISION_LOG_INFO("Median of error : " << statistics.median);

        std::vector<calibration::PointPair> points;
        if (!generatePoints(points, cameraPinhole, lineWithPoints))
        {
            ALICEVISION_LOG_ERROR("Error generating points");
            continue;
        }

        cameraPinhole->setWidth(w);
        cameraPinhole->setHeight(h);
        cameraPinhole->setScale(d, d);
        cameraPinhole->setOffset(hw, hh);
             

        //Estimate distortion
        if (std::dynamic_pointer_cast<camera::PinholeRadialK1>(cameraBase))
        {
            if (!estimateDistortionK1(cameraPinhole, statistics, points))
            {
                ALICEVISION_LOG_ERROR("Error estimating reverse distortion");
                continue;
            }
        }
        else if (std::dynamic_pointer_cast<camera::PinholeRadialK3>(cameraBase))
        {
            if (!estimateDistortionK3(cameraPinhole, statistics, points))
            {
                ALICEVISION_LOG_ERROR("Error estimating reverse distortion");
                continue;
            }
        }
        else if (std::dynamic_pointer_cast<camera::Pinhole3DERadial4>(cameraBase))
        {
            if (!estimateDistortion3DER4(cameraPinhole, statistics, points))
            {
                ALICEVISION_LOG_ERROR("Error estimating reverse distortion");
                continue;
            }
        }
        else if (std::dynamic_pointer_cast<camera::Pinhole3DEAnamorphic4>(cameraBase))
        {
            if (!estimateDistortion3DEA4(cameraPinhole, statistics, points))
            {
                ALICEVISION_LOG_ERROR("Error estimating reverse distortion");
                continue;
            }
        }
        else if (std::dynamic_pointer_cast<camera::Pinhole3DEClassicLD>(cameraBase))
        {
            if (!estimateDistortion3DELD(cameraPinhole, statistics, points))
            {
                ALICEVISION_LOG_ERROR("Error estimating reverse distortion");
                continue;
            }
        }
        else 
        {
            ALICEVISION_LOG_ERROR("Incompatible camera distortion model");
        }

        ALICEVISION_LOG_INFO("Result quality of inversion : ");
        ALICEVISION_LOG_INFO("Mean of error (stddev) : " << statistics.mean << "(" << statistics.stddev <<")");
        ALICEVISION_LOG_INFO("Median of error : " << statistics.median);

        Vec2 offset;
        image::Image<image::RGBColor> ud = undistort(offset, cameraPinhole, input);
        image::writeImage(undistortedImagePath, ud, image::EImageColorSpace::AUTO);

        image::Image<image::RGBfColor> stmap = undistortSTMAP(offset, cameraPinhole, input);
        image::writeImage(stMapImagePath, stmap, image::EImageColorSpace::NO_CONVERSION);
    }

    
    if(!sfmDataIO::Save(sfmData, sfmOutputDataFilepath, sfmDataIO::ESfMData(sfmDataIO::ALL)))
    {
        ALICEVISION_LOG_ERROR("The output SfMData file '" << sfmOutputDataFilepath << "' cannot be read.");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

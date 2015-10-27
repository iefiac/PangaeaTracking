#pragma once

#include "./Mesh.h"
#include "./ImagePyramid.h"

#include "sample.h"
#include "jet_extras.h"
#include "ceres/rotation.h"

enum baType{
    BA_MOT,
    BA_STR,
    BA_MOTSTR
};

enum dataTermErrorType{
    PE_INTENSITY = 0,
    PE_COLOR,
    PE_DEPTH,
    PE_DEPTH_PLANE,
    NUM_DATA_TERM_ERROR
};

static int PE_RESIDUAL_NUM_ARRAY[NUM_DATA_TERM_ERROR] = {1,3,3,1};

// need to write a new ResidualImageProjection(ResidualImageInterpolationProjection)
// for a data term similar to dynamicFusion
// optional parameters: vertex value(will be needed if we are using photometric)
// paramters: vertex position, neighboring weights, pCamera and pFrame
// optimization: rotation, translation and neighboring transformations
// (split up rotation and translation)
// several things we could try here:
// fix the number of neighbors, if we use knn nearest neighbors
// or use variable number of neighbors, if we use neighbors in a certain radius range
// arap term of dynamicFusion can be implemented as usual

// Image Projection residual covers all the possible projection cases
// Including gray, rgb, point-to-point and point-to-plane error
// For different pyramid level, just change the pCamera and pFrame accordingly,
// make sure that pCamera and pFrame are consistent

class ResidualImageProjection
{
public:
ResidualImageProjection(double weight, double* pValue, const CameraInfo* pCamera,
    const ImageLevel* pFrame, dataTermErrorType PE_TYPE=PE_INTENSITY):
    weight(weight), pValue(pValue), pCamera(pCamera),
        pFrame(pFrame), PE_TYPE(PE_TYPE)
    {}
ResidualImageProjection(double weight, const CameraInfo* pCamera,
    const ImageLevel* pFrame, dataTermErrorType PE_TYPE=PE_INTENSITY):
    weight(weight), pCamera(pCamera), pFrame(pFrame), PE_TYPE(PE_TYPE)
    {
        // check the consistency between camera and images
        assert(pCamera->width == pFrame->grayImage.cols);
        assert(pCamera->height == pFrame->grayImage.rows);
    }

    template <typename T>
        bool operator()(const T* const rotation,
            const T* const translation,
            const T* const xyz,
            T* residuals) const
    {
        int residual_num = PE_RESIDUAL_NUM_ARRAY[PE_TYPE];
        for(int i = 0; i < residual_num; ++i)
        residuals[i] = T(0.0);

        T p[3];
        T transformed_r, transformed_c;

        ceres::AngleAxisRotatePoint( rotation, xyz, p);
        p[0] += translation[0];
        p[1] += translation[1];
        p[2] += translation[2];

        //debug
        // double xyz_[3],rotation_[3],translation_[3];
        // double p_[3];
        // for(int i = 0; i < 3; ++i)
        // {
        //     xyz_[i] = ceres::JetOps<T>::GetScalar(xyz[i]);
        //     rotation_[i] = ceres::JetOps<T>::GetScalar(rotation[i]);
        //     translation_[i] = ceres::JetOps<T>::GetScalar(translation[i]);
        //     p_[i] = ceres::JetOps<T>::GetScalar(p[i]);
        // }


        if(pCamera->isOrthoCamera)
        {
            transformed_c = p[0]; //transformed x (2D)
            transformed_r = p[1]; //transformed y (2D)
        }
        else
        {
            transformed_c = ( ( p[0] * T( pCamera->KK[0][0] ) ) / p[2] ) + T( pCamera->KK[0][2] ); //transformed x (2D)
            transformed_r = ( ( p[1] * T( pCamera->KK[1][1] ) ) / p[2] ) + T( pCamera->KK[1][2] ); //transformed y (2D)
        }

        T templateValue, currentValue;

        if( transformed_r >= T(0.0) && transformed_r < T(pCamera->height) &&
            transformed_c >= T(0.0) && transformed_c < T(pCamera->width))
        {
            switch(PE_TYPE)
            {
                case PE_INTENSITY:
                {
                    templateValue = T(pValue[0]);
                    currentValue = SampleWithDerivative< T, InternalIntensityImageType > (pFrame->grayImage,
                        pFrame->gradXImage, pFrame->gradYImage, transformed_c, transformed_r );
                    residuals[0] = T(weight) * (currentValue - templateValue);
                    break;
                }
                case PE_COLOR:
                {
                    for(int i = 0; i < 3; ++i)
                    {
                        templateValue = T(pValue[i]);
                        currentValue = SampleWithDerivative< T, InternalIntensityImageType >( pFrame->colorImageSplit[i],
                            pFrame->colorImageGradXSplit[i], pFrame->colorImageGradYSplit[i], transformed_c, transformed_r );
                        residuals[i] = T(weight) * ( currentValue - templateValue );
                    }
                    break;
                }
                case PE_DEPTH:   // point-to-point error
                {
                    // depth value of the point
                    templateValue = T(xyz[2]);
                    // the depth value read from depth image at projected position
                    currentValue = SampleWithDerivative< T, InternalIntensityImageType > (pFrame->depthImage,
                        pFrame->depthGradXImage, pFrame->depthGradYImage, transformed_c, transformed_r );

                    if(pCamera->isOrthoCamera) // if this is really a orthographic camera
                    {
                        residuals[0] = T(weight) * (currentValue - templateValue);
                        residuals[1] = T(0.0); residuals[2] = T(0.0);
                    }else
                    {
                        residuals[2] = T(currentValue - templateValue);
                        residuals[0] = residuals[2] *
                            (pCamera->invKK[0][0]*transformed_c + pCamera->invKK[0][2]);
                        residuals[1] = residuals[2] *
                            (pCamera->invKK[1][1]*transformed_r + pCamera->invKK[1][2]);
                    }
                    break;
                }
                case PE_DEPTH_PLANE: // point-to-plane error
                {
                    //
                    T back_projection[3];
                    currentValue = SampleWithDerivative< T, InternalIntensityImageType > (pFrame->depthImage,
                        pFrame->depthGradXImage, pFrame->depthGradYImage, transformed_c, transformed_r );
                    back_projection[2] = currentValue;
                    back_projection[0] = back_projection[2] * (pCamera->invKK[0][0]*transformed_c + pCamera->invKK[0][2]);
                    back_projection[1] = back_projection[2] * (pCamera->invKK[1][1]*transformed_r + pCamera->invKK[1][2]);

                    // normals at back projection point
                    T normals_at_bp[3];
                    for(int  i = 0; i < 3; ++i)
                    {
                        normals_at_bp[i] = SampleWithDerivative< T, InternalIntensityImageType > (pFrame->depthNormalImageSplit[i],
                            pFrame->depthNormalImageGradXSplit[i], pFrame->depthNormalImageGradYSplit[i], transformed_c, transformed_r );
                    }

                    // should we do normalization on the normals, probably doesn't make much difference
                    // as the normals has already been normalized and we just do
                    // interpolation

                    // get the point-to-plane error
                    residuals[0] = normals_at_bp[0]*(xyz[0] - back_projection[0]) +
                        normals_at_bp[1]*(xyz[1] - back_projection[1]) +
                        normals_at_bp[2]*(xyz[2] - back_projection[2]);
                }
            }
        }
        return true;
    }

private:
    double weight;
    // this will only be useful if we are using gray or rgb value
    // give a dummy value in other cases
    double* pValue;
    const CameraInfo* pCamera;
    const ImageLevel* pFrame;
    dataTermErrorType PE_TYPE;
};

// just for fun, get the numeric differentiation and compare the results
// with autodifferentiation


class ResidualTV
{
public:

ResidualTV(double weight, double* pVertex, double* pNeighbor):
    weight(weight), pVertex(pVertex), pNeighbor(pNeighbor) {}

    template <typename T>
        bool operator()(const T* const pCurrentVertex,
            const T* const pCurrentNeighbor,
            T* residuals) const
    {
        for(int i = 0; i <3; ++i)
        {
            residuals[i] = T(weight) * ( T(pVertex[i]  - pNeighbor[i]) -
                ( pCurrentVertex[i] - pCurrentNeighbor[i]) );
        }
        return true;
    }

private:

    double weight;
    const double* pVertex;
    const double* pNeighbor;

};

// total variation on top of the local rotations
class ResidualRotTV
{
public:

ResidualRotTV(double weight):weight(weight){}

    template<typename T>
        bool operator()(const T* const pCurrentRot,
            const T* const pCurrentNeighbor,
            T* residuals) const
    {
        for(int i= 0; i < 3; ++i)
        {
            residuals[i] = T(weight) * ( pCurrentRot[i] - pCurrentNeighbor[i] );
        }

        return true;
    }

private:

    double weight;
};

class ResidualINEXTENT
{
public:

    ResidualINEXTENT(double weight, double* pVertex, double* pNeighbor):
        weight(weight),pVertex(pVertex),pNeighbor(pNeighbor)
    {}

    template <typename T>
        bool operator()(const T* const pCurrentVertex,
            const T* const pCurrentNeighbor,
            T* residuals) const
    {
        T diff[3];
        T diffref[3];

        for(int i = 0; i < 3; ++i)
        {
            diff[i] = pCurrentVertex[i] - pCurrentNeighbor[i];
            diffref[i] = T(pVertex[i] - pNeighbor[i]);
        }

        T length;
        T lengthref;

        length = sqrt(diff[0] * diff[0] +
            diff[1] * diff[1] +
            diff[2] * diff[2]);

        lengthref = sqrt(diffref[0] * diffref[0] +
            diffref[1] * diffref[1] +
            diffref[2] * diffref[2]);

        residuals[0] = T(weight) * (lengthref - length);

        return true;

    }

private:

    double weight;
    const double* pVertex;
    const double* pNeighbor;

};

// the rotation to be optimized is from template mesh to current mesh
class ResidualARAP
{
public:

ResidualARAP(double weight, double* pVertex, double* pNeighbor):
    weight(weight), pVertex(pVertex), pNeighbor(pNeighbor) {}

    template <typename T>
        bool operator()(const T* const pCurrentVertex,
            const T* const pCurrentNeighbor,
            const T* const pRotVertex,
            T* residuals) const
    {
        T templateDiff[3];
        T rotTemplateDiff[3];
        T currentDiff[3];

        for(int i = 0; i <3; ++i)
        {
            templateDiff[i] =  T(pVertex[i] - pNeighbor[i]);
            currentDiff[i]  = pCurrentVertex[i] - pCurrentNeighbor[i];
        }

        ceres::AngleAxisRotatePoint(pRotVertex, templateDiff, rotTemplateDiff);

        residuals[0] = T(weight) * ( currentDiff[0] - rotTemplateDiff[0] );
        residuals[1] = T(weight) * ( currentDiff[1] - rotTemplateDiff[1] );
        residuals[2] = T(weight) * ( currentDiff[2] - rotTemplateDiff[2] );

        return true;

    }

private:

    double weight;
    const double* pVertex;
    const double* pNeighbor;

};

class ResidualDeform
{
public:
ResidualDeform(double weight, double* pVertex):
    weight(weight), pVertex(pVertex){}

    template <typename T>
        bool operator()(const T* const pCurrentVertex,
            T* residuals) const
    {
        for(int i = 0; i < 3; ++i)
        {
            residuals[i] = T(weight) * (pCurrentVertex[i] - T(pVertex[i]));
        }
        return true;
    }

private:

    double weight;
    const double* pVertex;

};

class ResidualTemporalMotion
{
public:
    ResidualTemporalMotion(double* pPrevRot, double* pPrevTrans,
        double rotWeight, double transWeight):
        pPrevRot(pPrevRot), pPrevTrans(pPrevTrans),
        rotWeight(rotWeight), transWeight(transWeight)
    {}

    template <typename T>
        bool operator()(const T* const pRot,
            const T* const pTrans,
            T* residuals) const
    {
        residuals[0] = rotWeight * (pRot[0] -     pPrevRot[0]);
        residuals[1] = rotWeight * (pRot[1] -     pPrevRot[1]);
        residuals[2] = rotWeight * (pRot[2] -     pPrevRot[2]);
        residuals[3] = transWeight * (pTrans[0] - pPrevTrans[0]);
        residuals[4] = transWeight * (pTrans[1] - pPrevTrans[1]);
        residuals[5] = transWeight * (pTrans[2] - pPrevTrans[2]);

        cout << "inside printing started" << endl;

        cout << "transWeight: " << transWeight << endl;
        cout << "rotWeight: " << rotWeight << endl;
        
        cout << pRot[0] << " " << pRot[1] << " " << pRot[2] << endl;
        cout << pTrans[0] << " " << pTrans[1] << " " << pTrans[2] << endl;
        cout << endl;
        cout << pPrevRot[0] << " " << pPrevRot[1] << " " << pPrevRot[2] << endl;
        cout << pPrevTrans[0] << " " << pPrevTrans[1] << " " << pPrevTrans[2] << endl;
        cout << "inside printing finished" << endl;

        return true;
    }

    double rotWeight;
    double transWeight;
    const double* pPrevRot;
    const double* pPrevTrans;

};


class EnergyCallback: public ceres::IterationCallback
{

private:
    std::vector<double> m_EnergyRecord;

public:
    EnergyCallback(){}
    virtual ceres::CallbackReturnType operator()(const ceres::IterationSummary & summary)
    {
        m_EnergyRecord.push_back(summary.cost);
        return ceres::SOLVER_CONTINUE;
    }
    void PrintEnergy(std::ostream& output)
    {
        output << "Energy Started" << std::endl;
        for(int i=0; i< m_EnergyRecord.size(); ++i)
        output<<(i+1)<<" "<< m_EnergyRecord[i]<<std::endl;
        output << "Energy Ended" << std::endl;
    }
    void Reset()
    {
        m_EnergyRecord.clear();
    }

};
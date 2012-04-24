/*  Copyright 2011, 2012 The Ready Bunch

    This file is part of Ready.

    Ready is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ready is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ready. If not, see <http://www.gnu.org/licenses/>.         */

// local:
#include "ImageRD.hpp"
#include "utils.hpp"
#include "overlays.hpp"
#include "Properties.hpp"
#include "IO_XML.hpp"

// stdlib:
#include <stdlib.h>
#include <math.h>

// STL:
#include <cassert>
#include <stdexcept>
using namespace std;

// VTK:
#include <vtkImageData.h>
#include <vtkImageAppendComponents.h>
#include <vtkImageExtractComponents.h>
#include <vtkPointData.h>
#include <vtkXMLUtilities.h>
#include <vtkCellData.h>
#include <vtkImageWrapPad.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkCamera.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkSmartPointer.h>
#include <vtkContourFilter.h>
#include <vtkProperty.h>
#include <vtkImageActor.h>
#include <vtkProperty2D.h>
#include <vtkRendererCollection.h>
#include <vtkLookupTable.h>
#include <vtkImageMapToColors.h>
#include <vtkScalarBarActor.h>
#include <vtkCubeSource.h>
#include <vtkExtractEdges.h>
#include <vtkWarpScalar.h>
#include <vtkImageDataGeometryFilter.h>
#include <vtkPolyDataNormals.h>
#include <vtkImageMirrorPad.h>
#include <vtkCubeAxesActor2D.h>
#include <vtkImageReslice.h>
#include <vtkMatrix4x4.h>
#include <vtkMath.h>
#include <vtkTextActor3D.h>
#include <vtkThreshold.h>
#include <vtkGeometryFilter.h>
#include <vtkTransform.h>
#include <vtkTransformFilter.h>
#include <vtkXMLImageDataWriter.h>
#include <vtkObjectFactory.h>
#include <vtkFloatArray.h>

// -------------------------------------------------------------------

/// Used to write vtkImageData to XML, with an added RD section containing rule information.
class RD_XMLImageWriter : public vtkXMLImageDataWriter
{
    public:

        vtkTypeMacro(RD_XMLImageWriter, vtkXMLImageDataWriter);
        static RD_XMLImageWriter* New();

        void SetSystem(const ImageRD* rd_system);
        void SetRenderSettings(const Properties* settings) { this->render_settings = settings; }

    protected:  

        RD_XMLImageWriter() : system(NULL) {} 

        static vtkSmartPointer<vtkXMLDataElement> BuildRDSystemXML(ImageRD* system);

        virtual int WritePrimaryElement(ostream& os,vtkIndent indent);

    protected:

        const ImageRD* system;
        const Properties* render_settings;
};

// ---------------------------------------------------------------------

ImageRD::ImageRD()
{
    this->image_wrap_pad_filter = NULL;
    this->starting_pattern = vtkImageData::New();
}

// ---------------------------------------------------------------------

ImageRD::~ImageRD()
{
    this->DeallocateImages();
    this->starting_pattern->Delete();
}

// ---------------------------------------------------------------------

void ImageRD::DeallocateImages()
{
    for(int iChem=0;iChem<(int)this->images.size();iChem++)
    {
        if(this->images[iChem])
            this->images[iChem]->Delete();
    }
}

// ---------------------------------------------------------------------

int ImageRD::GetArenaDimensionality() const
{
    assert(this->images.front());
    int dimensionality=0;
    for(int iDim=0;iDim<3;iDim++)
        if(this->images.front()->GetDimensions()[iDim]>1)
            dimensionality++;
    return dimensionality;
}

// ---------------------------------------------------------------------

float ImageRD::GetX() const
{
    return this->images.front()->GetDimensions()[0];
}

// ---------------------------------------------------------------------

float ImageRD::GetY() const
{
    return this->images.front()->GetDimensions()[1];
}

// ---------------------------------------------------------------------

float ImageRD::GetZ() const
{
    return this->images.front()->GetDimensions()[2];
}

// ---------------------------------------------------------------------

vtkImageData* ImageRD::GetImage(int iChemical) const
{ 
    return this->images[iChemical];
}

// ---------------------------------------------------------------------

vtkSmartPointer<vtkImageData> ImageRD::GetImage() const
{ 
    vtkSmartPointer<vtkImageAppendComponents> iac = vtkSmartPointer<vtkImageAppendComponents>::New();
    for(int i=0;i<this->GetNumberOfChemicals();i++)
        iac->AddInput(this->GetImage(i));
    iac->Update();
    return iac->GetOutput();
}


// ---------------------------------------------------------------------

void ImageRD::CopyFromImage(vtkImageData* im)
{
    int n_arrays = im->GetPointData()->GetNumberOfArrays();
    int n_components = im->GetNumberOfScalarComponents();

    if(n_components==1 && n_arrays==this->GetNumberOfChemicals())
    {
        // convert named array data to single-component data in multiple images
        for(int iChem=0;iChem<this->GetNumberOfChemicals();iChem++)
        {
            this->images[iChem]->SetExtent(im->GetExtent());
            this->images[iChem]->GetPointData()->SetScalars(im->GetPointData()->GetArray(GetChemicalName(iChem).c_str()));
        }
    }
    else if(n_arrays==1 && n_components==this->GetNumberOfChemicals()) 
    {
        // convert multi-component data to single-component data in multiple images
        vtkSmartPointer<vtkImageExtractComponents> iec = vtkSmartPointer<vtkImageExtractComponents>::New();
        iec->SetInput(im);
        for(int i=0;i<this->GetNumberOfChemicals();i++)
        {
            iec->SetComponents(i);
            iec->Update();
            this->images[i]->DeepCopy(iec->GetOutput());
        }
    }
    else   
        throw runtime_error("ImageRD::CopyFromImage : chemical count mismatch");
    UpdateImageWrapPadFilter();
}

// ---------------------------------------------------------------------

void ImageRD::AllocateImages(int x,int y,int z,int nc)
{
    this->DeallocateImages();
    this->n_chemicals = nc;
    this->images.resize(nc);
    for(int i=0;i<nc;i++)
        this->images[i] = AllocateVTKImage(x,y,z);
    this->is_modified = true;
}

// ---------------------------------------------------------------------

/* static */ vtkImageData* ImageRD::AllocateVTKImage(int x,int y,int z)
{
    vtkImageData *im = vtkImageData::New();
    assert(im);
    im->SetNumberOfScalarComponents(1);
    im->SetScalarTypeToFloat();
    im->SetDimensions(x,y,z);
    im->AllocateScalars();
    if(im->GetDimensions()[0]!=x || im->GetDimensions()[1]!=y || im->GetDimensions()[2]!=z)
        throw runtime_error("ImageRD::AllocateVTKImage : Failed to allocate image data - dimensions too big?");
    return im;
}

// ---------------------------------------------------------------------

void ImageRD::GenerateInitialPattern()
{
    this->BlankImage();

    const int X = this->images.front()->GetDimensions()[0];
    const int Y = this->images.front()->GetDimensions()[1];
    const int Z = this->images.front()->GetDimensions()[2];

    for(int z=0;z<Z;z++)
    {
        for(int y=0;y<Y;y++)
        {
            for(int x=0;x<X;x++)
            {
                for(int iOverlay=0;iOverlay<(int)this->initial_pattern_generator.size();iOverlay++)
                {
                    Overlay* overlay = this->initial_pattern_generator[iOverlay];

                    int iC = overlay->GetTargetChemical();
                    if(iC<0 || iC>=this->GetNumberOfChemicals())
                        throw runtime_error("Overlay: chemical out of range: "+GetChemicalName(iC));

                    float *val = vtk_at(static_cast<float*>(this->GetImage(iC)->GetScalarPointer()),x,y,z,X,Y);
                    vector<float> vals(this->GetNumberOfChemicals());
                    for(int i=0;i<this->GetNumberOfChemicals();i++)
                        vals[i] = *vtk_at(static_cast<float*>(this->GetImage(i)->GetScalarPointer()),x,y,z,X,Y);
                    *val = overlay->Apply(vals,this,x,y,z);
                }
            }
        }
    }
    for(int i=0;i<(int)this->images.size();i++)
        this->images[i]->Modified();
    UpdateImageWrapPadFilter();
    this->timesteps_taken = 0;
}

// ---------------------------------------------------------------------

void ImageRD::BlankImage()
{
    for(int iImage=0;iImage<(int)this->images.size();iImage++)
    {
        this->images[iImage]->GetPointData()->GetScalars()->FillComponent(0,0.0);
        this->images[iImage]->Modified();
    }
    this->timesteps_taken = 0;
}

// ---------------------------------------------------------------------

void ImageRD::Update(int n_steps)
{
    this->InternalUpdate(n_steps);

    this->timesteps_taken += n_steps;

    for(int ic=0;ic<this->GetNumberOfChemicals();ic++)
        this->images[ic]->Modified();

    UpdateImageWrapPadFilter();
}

// ---------------------------------------------------------------------

void ImageRD::UpdateImageWrapPadFilter()
{
    // kludgy workaround for the GenerateCubesFromLabels approach not being fully pipelined
    if(this->image_wrap_pad_filter)
    {
        this->image_wrap_pad_filter->Update();
        this->image_wrap_pad_filter->GetOutput()->GetCellData()->SetScalars(
            dynamic_cast<vtkImageData*>(this->image_wrap_pad_filter->GetInput())->GetPointData()->GetScalars());
    }
}

// ---------------------------------------------------------------------

void ImageRD::InitializeRenderPipeline(vtkRenderer* pRenderer,const Properties& render_settings)
{
    this->SetImageWrapPadFilter(NULL); // workaround for the GenerateCubesFromLabels approach not being fully pipelined
    switch(this->GetArenaDimensionality())
    {
        // TODO: merge the dimensionalities (often want one/more slices from lower dimensionalities)
        case 1: this->InitializeVTKPipeline_1D(pRenderer,render_settings); break;
        case 2: this->InitializeVTKPipeline_2D(pRenderer,render_settings); break;
        case 3: this->InitializeVTKPipeline_3D(pRenderer,render_settings); break;
        default:
            throw runtime_error("ImageRD::InitializeRenderPipeline : Unsupported dimensionality");
    }
}

// ---------------------------------------------------------------------

void ImageRD::InitializeVTKPipeline_1D(vtkRenderer* pRenderer,const Properties& render_settings)
{
    float low = render_settings.GetProperty("low").GetFloat();
    float high = render_settings.GetProperty("high").GetFloat();
    float vertical_scale_1D = render_settings.GetProperty("vertical_scale_1D").GetFloat();
    float r,g,b,low_hue,low_sat,low_val,high_hue,high_sat,high_val;
    render_settings.GetProperty("color_low").GetColor(r,g,b);
    vtkMath::RGBToHSV(r,g,b,&low_hue,&low_sat,&low_val);
    render_settings.GetProperty("color_high").GetColor(r,g,b);
    vtkMath::RGBToHSV(r,g,b,&high_hue,&high_sat,&high_val);
    bool use_image_interpolation = render_settings.GetProperty("use_image_interpolation").GetBool();
    int iActiveChemical = IndexFromChemicalName(render_settings.GetProperty("active_chemical").GetChemical());
    float contour_level = render_settings.GetProperty("contour_level").GetFloat();
    bool use_wireframe = render_settings.GetProperty("use_wireframe").GetBool();
    bool show_multiple_chemicals = render_settings.GetProperty("show_multiple_chemicals").GetBool();

    int iFirstChem=0,iLastChem=this->GetNumberOfChemicals();
    if(!show_multiple_chemicals) { iFirstChem = iActiveChemical; iLastChem = iFirstChem+1; }
    
    float scaling = vertical_scale_1D / (high-low); // vertical_scale gives the height of the graph in worldspace units
    
    // create a lookup table for mapping values to colors
    vtkSmartPointer<vtkLookupTable> lut = vtkSmartPointer<vtkLookupTable>::New();
    lut->SetRampToLinear();
    lut->SetScaleToLinear();
    lut->SetTableRange(low,high);
    lut->SetSaturationRange(low_sat,high_sat);
    lut->SetHueRange(low_hue,high_hue);
    lut->SetValueRange(low_val,high_val);

    // pad the image a little so we can actually see it as a 2D strip rather than being invisible
    vtkSmartPointer<vtkImageMirrorPad> pad = vtkSmartPointer<vtkImageMirrorPad>::New();
    pad->SetInput(this->GetImage(iActiveChemical));
    pad->SetOutputWholeExtent(0,this->GetX()-1,-1,this->GetY(),0,this->GetZ()-1);

    // pass the image through the lookup table
    vtkSmartPointer<vtkImageMapToColors> image_mapper = vtkSmartPointer<vtkImageMapToColors>::New();
    image_mapper->SetLookupTable(lut);
    image_mapper->SetInputConnection(pad->GetOutputPort());
  
    // an actor determines how a scene object is displayed
    vtkSmartPointer<vtkImageActor> actor = vtkSmartPointer<vtkImageActor>::New();
    actor->SetInput(image_mapper->GetOutput());
    actor->SetPosition(0,low*scaling - 5.0,0);
    if(!use_image_interpolation)
        actor->InterpolateOff();
    pRenderer->AddActor(actor);

    // also add a scalar bar to show how the colors correspond to values
    vtkSmartPointer<vtkScalarBarActor> scalar_bar = vtkSmartPointer<vtkScalarBarActor>::New();
    scalar_bar->SetLookupTable(lut);
    pRenderer->AddActor2D(scalar_bar);

    // add a line graph for all the chemicals (active one highlighted)
    for(int iChemical=iFirstChem;iChemical<iLastChem;iChemical++)
    {
        vtkSmartPointer<vtkImageDataGeometryFilter> plane = vtkSmartPointer<vtkImageDataGeometryFilter>::New();
        plane->SetInput(this->GetImage(iChemical));
        vtkSmartPointer<vtkWarpScalar> warp = vtkSmartPointer<vtkWarpScalar>::New();
        warp->SetInputConnection(plane->GetOutputPort());
        warp->SetScaleFactor(-scaling);
        vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputConnection(warp->GetOutputPort());
        mapper->ScalarVisibilityOff();
        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetAmbient(1);
        if(iChemical==iActiveChemical)
            actor->GetProperty()->SetColor(1,1,1);
        else
            actor->GetProperty()->SetColor(0.5,0.5,0.5);
        actor->RotateX(90.0);
        pRenderer->AddActor(actor);
    }
    
    // add an axis
    vtkSmartPointer<vtkCubeAxesActor2D> axis = vtkSmartPointer<vtkCubeAxesActor2D>::New();
    axis->SetCamera(pRenderer->GetActiveCamera());
    axis->SetBounds(0,0,low*scaling,high*scaling,0,0);
    axis->SetRanges(0,0,low,high,0,0);
    axis->UseRangesOn();
    axis->XAxisVisibilityOff();
    axis->ZAxisVisibilityOff();
    axis->SetYLabel("");
    axis->SetLabelFormat("%.2f");
    axis->SetInertia(10000);
    axis->SetCornerOffset(0);
    axis->SetNumberOfLabels(5);
    pRenderer->AddActor(axis);
}

// ---------------------------------------------------------------------

void ImageRD::InitializeVTKPipeline_2D(vtkRenderer* pRenderer,const Properties& render_settings)
{
    float low = render_settings.GetProperty("low").GetFloat();
    float high = render_settings.GetProperty("high").GetFloat();
    float vertical_scale_2D = render_settings.GetProperty("vertical_scale_2D").GetFloat();
    float r,g,b,low_hue,low_sat,low_val,high_hue,high_sat,high_val;
    render_settings.GetProperty("color_low").GetColor(r,g,b);
    vtkMath::RGBToHSV(r,g,b,&low_hue,&low_sat,&low_val);
    render_settings.GetProperty("color_high").GetColor(r,g,b);
    vtkMath::RGBToHSV(r,g,b,&high_hue,&high_sat,&high_val);
    bool use_image_interpolation = render_settings.GetProperty("use_image_interpolation").GetBool();
    int iActiveChemical = IndexFromChemicalName(render_settings.GetProperty("active_chemical").GetChemical());
    float contour_level = render_settings.GetProperty("contour_level").GetFloat();
    bool use_wireframe = render_settings.GetProperty("use_wireframe").GetBool();
    float surface_r,surface_g,surface_b;
    render_settings.GetProperty("surface_color").GetColor(surface_r,surface_g,surface_b);
    bool show_multiple_chemicals = render_settings.GetProperty("show_multiple_chemicals").GetBool();
    bool show_displacement_mapped_surface = render_settings.GetProperty("show_displacement_mapped_surface").GetBool();
    
    float scaling = vertical_scale_2D / (high-low); // vertical_scale gives the height of the graph in worldspace units

    vtkFloatingPointType offset[3] = {0,0,0};

    int iFirstChem=0,iLastChem=this->GetNumberOfChemicals();
    if(!show_multiple_chemicals) { iFirstChem = iActiveChemical; iLastChem = iFirstChem+1; }
    
    // create a lookup table for mapping values to colors
    vtkSmartPointer<vtkLookupTable> lut = vtkSmartPointer<vtkLookupTable>::New();
    lut->SetRampToLinear();
    lut->SetScaleToLinear();
    lut->SetTableRange(low,high);
    lut->SetSaturationRange(low_sat,high_sat);
    lut->SetHueRange(low_hue,high_hue);
    lut->SetValueRange(low_val,high_val);

    for(int iChem=iFirstChem;iChem<iLastChem;iChem++)
    {
        // add a color-mapped image
        {
            // pass the image through the lookup table
            vtkSmartPointer<vtkImageMapToColors> image_mapper = vtkSmartPointer<vtkImageMapToColors>::New();
            image_mapper->SetLookupTable(lut);
            image_mapper->SetInput(this->GetImage(iChem));

            vtkSmartPointer<vtkImageActor> actor = vtkSmartPointer<vtkImageActor>::New();
            actor->SetInput(image_mapper->GetOutput());
            if(!use_image_interpolation)
                actor->InterpolateOff();
            actor->SetPosition(offset[0],offset[1]-this->GetY()-3,offset[2]);

            pRenderer->AddActor(actor);
        }

        if(show_displacement_mapped_surface)
        {
            vtkSmartPointer<vtkImageDataGeometryFilter> plane = vtkSmartPointer<vtkImageDataGeometryFilter>::New();
            plane->SetInput(this->GetImage(iChem));
            vtkSmartPointer<vtkWarpScalar> warp = vtkSmartPointer<vtkWarpScalar>::New();
            warp->SetInputConnection(plane->GetOutputPort());
            warp->SetScaleFactor(scaling);
            vtkSmartPointer<vtkPolyDataNormals> normals = vtkSmartPointer<vtkPolyDataNormals>::New();
            normals->SetInputConnection(warp->GetOutputPort());
            normals->SplittingOff();
            vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputConnection(normals->GetOutputPort());
            mapper->ScalarVisibilityOff();
            vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
            actor->SetMapper(mapper);
            actor->GetProperty()->SetColor(surface_r,surface_g,surface_b);
            if(use_wireframe)
                actor->GetProperty()->SetRepresentationToWireframe();
            actor->SetPosition(offset);
            pRenderer->AddActor(actor);

            // add the bounding box
            {
                vtkSmartPointer<vtkCubeSource> box = vtkSmartPointer<vtkCubeSource>::New();
                box->SetBounds(0,this->GetX(),0,this->GetY(),low*scaling,high*scaling);

                vtkSmartPointer<vtkExtractEdges> edges = vtkSmartPointer<vtkExtractEdges>::New();
                edges->SetInputConnection(box->GetOutputPort());

                vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
                mapper->SetInputConnection(edges->GetOutputPort());

                vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
                actor->SetMapper(mapper);
                actor->GetProperty()->SetColor(0,0,0);  
                actor->GetProperty()->SetAmbient(1);
                actor->SetPosition(offset);

                pRenderer->AddActor(actor);
            }
        }

        // add a text label
        if(this->GetNumberOfChemicals()>1)
        {
            vtkSmartPointer<vtkTextActor3D> label = vtkSmartPointer<vtkTextActor3D>::New();
            label->SetInput(GetChemicalName(iChem).c_str());
            label->SetPosition(offset[0]+this->GetX()/2,offset[1]-this->GetY()-20,offset[2]);
            pRenderer->AddActor(label);
        }

        offset[0] += this->GetX()+5; // the next chemical should appear further to the right
    }

    if(show_displacement_mapped_surface)
    {
        // add an axis
        vtkSmartPointer<vtkCubeAxesActor2D> axis = vtkSmartPointer<vtkCubeAxesActor2D>::New();
        axis->SetCamera(pRenderer->GetActiveCamera());
        axis->SetBounds(0,0,this->GetY(),this->GetY(),low*scaling,high*scaling);
        axis->SetRanges(0,0,0,0,low,high);
        axis->UseRangesOn();
        axis->XAxisVisibilityOff();
        axis->YAxisVisibilityOff();
        axis->SetZLabel("");
        axis->SetLabelFormat("%.2f");
        axis->SetInertia(10000);
        axis->SetCornerOffset(0);
        axis->SetNumberOfLabels(5);
        pRenderer->AddActor(axis);
    }

    // add a scalar bar to show how the colors correspond to values
    vtkSmartPointer<vtkScalarBarActor> scalar_bar = vtkSmartPointer<vtkScalarBarActor>::New();
    scalar_bar->SetLookupTable(lut);
    pRenderer->AddActor2D(scalar_bar);
}

// ---------------------------------------------------------------------

void ImageRD::InitializeVTKPipeline_3D(vtkRenderer* pRenderer,const Properties& render_settings)
{
    float low = render_settings.GetProperty("low").GetFloat();
    float high = render_settings.GetProperty("high").GetFloat();
    float r,g,b,low_hue,low_sat,low_val,high_hue,high_sat,high_val;
    render_settings.GetProperty("color_low").GetColor(r,g,b);
    vtkMath::RGBToHSV(r,g,b,&low_hue,&low_sat,&low_val);
    render_settings.GetProperty("color_high").GetColor(r,g,b);
    vtkMath::RGBToHSV(r,g,b,&high_hue,&high_sat,&high_val);
    bool use_image_interpolation = render_settings.GetProperty("use_image_interpolation").GetBool();
    int iActiveChemical = IndexFromChemicalName(render_settings.GetProperty("active_chemical").GetChemical());
    float contour_level = render_settings.GetProperty("contour_level").GetFloat();
    bool use_wireframe = render_settings.GetProperty("use_wireframe").GetBool();
    bool slice_3D = render_settings.GetProperty("slice_3D").GetBool();
    string slice_3D_axis = render_settings.GetProperty("slice_3D_axis").GetAxis();
    float slice_3D_position = render_settings.GetProperty("slice_3D_position").GetFloat();
    float surface_r,surface_g,surface_b;
    render_settings.GetProperty("surface_color").GetColor(surface_r,surface_g,surface_b);

    // contour the 3D volume and render as a polygonal surface

    vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    if(use_image_interpolation)
    {
        // turns the 3d grid of sampled values into a polygon mesh for rendering,
        // by making a surface that contours the volume at a specified level    
        vtkSmartPointer<vtkContourFilter> surface = vtkSmartPointer<vtkContourFilter>::New();
        surface->SetInput(this->GetImage(iActiveChemical));
        surface->SetValue(0, contour_level);

        // a mapper converts scene objects to graphics primitives
        mapper->SetInputConnection(surface->GetOutputPort());
    }
    else
    {
        // render as cubes, Minecraft-style
        vtkImageData *image = this->GetImage(iActiveChemical);
        int *extent = image->GetExtent();

        vtkSmartPointer<vtkImageWrapPad> pad = vtkSmartPointer<vtkImageWrapPad>::New();
        pad->SetInput(image);
        pad->SetOutputWholeExtent(extent[0],extent[1]+1,extent[2],extent[3]+1,extent[4],extent[5]+1);
        pad->Update();
        pad->GetOutput()->GetCellData()->SetScalars(image->GetPointData()->GetScalars()); // a non-pipelined operation

        this->SetImageWrapPadFilter(pad); // workaround for the GenerateCubesFromLabels approach not being fully pipelined

        vtkSmartPointer<vtkThreshold> threshold = vtkSmartPointer<vtkThreshold>::New();
        threshold->SetInputConnection(pad->GetOutputPort());
        threshold->SetInputArrayToProcess(0, 0, 0,
            vtkDataObject::FIELD_ASSOCIATION_CELLS,
            vtkDataSetAttributes::SCALARS);
        threshold->ThresholdByUpper(contour_level);

        vtkSmartPointer<vtkTransform> transform = vtkSmartPointer<vtkTransform>::New();
        transform->Translate (-.5, -.5, -.5);
        vtkSmartPointer<vtkTransformFilter> transformModel = vtkSmartPointer<vtkTransformFilter>::New();
        transformModel->SetTransform(transform);
        transformModel->SetInputConnection(threshold->GetOutputPort());

        vtkSmartPointer<vtkGeometryFilter> geometry = vtkSmartPointer<vtkGeometryFilter>::New();
        geometry->SetInputConnection(transformModel->GetOutputPort());
        mapper->SetInputConnection(geometry->GetOutputPort());
    }
    mapper->ScalarVisibilityOff();

    // an actor determines how a scene object is displayed
    vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(surface_r,surface_g,surface_b);  
    actor->GetProperty()->SetAmbient(0.1);
    actor->GetProperty()->SetDiffuse(0.7);
    actor->GetProperty()->SetSpecular(0.2);
    actor->GetProperty()->SetSpecularPower(3);
    if(use_wireframe)
        actor->GetProperty()->SetRepresentationToWireframe();
    vtkSmartPointer<vtkProperty> bfprop = vtkSmartPointer<vtkProperty>::New();
    actor->SetBackfaceProperty(bfprop);
    bfprop->SetColor(0.3,0.3,0.3);
    bfprop->SetAmbient(0.3);
    bfprop->SetDiffuse(0.6);
    bfprop->SetSpecular(0.1);

    // add the actor to the renderer's scene
    pRenderer->AddActor(actor);

    // add the bounding box
    {
        vtkSmartPointer<vtkCubeSource> box = vtkSmartPointer<vtkCubeSource>::New();
        box->SetBounds(this->GetImage(iActiveChemical)->GetBounds());

        vtkSmartPointer<vtkExtractEdges> edges = vtkSmartPointer<vtkExtractEdges>::New();
        edges->SetInputConnection(box->GetOutputPort());

        vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputConnection(edges->GetOutputPort());

        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0,0,0);  
        actor->GetProperty()->SetAmbient(1);

        pRenderer->AddActor(actor);
    }

    // add a 2D slice too
    if(slice_3D)
    {
        // create a lookup table for mapping values to colors
        vtkSmartPointer<vtkLookupTable> lut = vtkSmartPointer<vtkLookupTable>::New();
        lut->SetRampToLinear();
        lut->SetScaleToLinear();
        lut->SetTableRange(low,high);
        lut->SetSaturationRange(low_sat,high_sat);
        lut->SetHueRange(low_hue,high_hue);
        lut->SetValueRange(low_val,high_val);

        // extract a slice
        vtkImageData *image = this->GetImage(iActiveChemical);
        // Matrices for axial, coronal, sagittal, oblique view orientations
        static double sagittalElements[16] = { // x
               0, 0,-1, 0,
               1, 0, 0, 0,
               0,-1, 0, 0,
               0, 0, 0, 1 };
        static double coronalElements[16] = { // y
                 1, 0, 0, 0,
                 0, 0, 1, 0,
                 0,-1, 0, 0,
                 0, 0, 0, 1 };
        static double axialElements[16] = { // z
                 1, 0, 0, 0,
                 0, 1, 0, 0,
                 0, 0, 1, 0,
                 0, 0, 0, 1 };
        /*static double obliqueElements[16] = { // could get fancy and have slanting slices
                 1, 0, 0, 0,
                 0, 0.866025, -0.5, 0,
                 0, 0.5, 0.866025, 0,
                 0, 0, 0, 1 };*/
        // Set the slice orientation
        vtkSmartPointer<vtkMatrix4x4> resliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
        if(slice_3D_axis=="x")
            resliceAxes->DeepCopy(sagittalElements);
        else if(slice_3D_axis=="y")
            resliceAxes->DeepCopy(coronalElements);
        else if(slice_3D_axis=="z")
            resliceAxes->DeepCopy(axialElements);
        resliceAxes->SetElement(0, 3, slice_3D_position * this->GetX());
        resliceAxes->SetElement(1, 3, slice_3D_position * this->GetY());
        resliceAxes->SetElement(2, 3, slice_3D_position * this->GetZ());

        vtkSmartPointer<vtkImageReslice> voi = vtkSmartPointer<vtkImageReslice>::New();
        voi->SetInput(image);
        voi->SetOutputDimensionality(2);
        voi->SetResliceAxes(resliceAxes);

        // pass the image through the lookup table
        vtkSmartPointer<vtkImageMapToColors> image_mapper = vtkSmartPointer<vtkImageMapToColors>::New();
        image_mapper->SetLookupTable(lut);
        image_mapper->SetInputConnection(voi->GetOutputPort());
      
        // an actor determines how a scene object is displayed
        vtkSmartPointer<vtkImageActor> actor = vtkSmartPointer<vtkImageActor>::New();
        actor->SetInput(image_mapper->GetOutput());
        actor->SetUserMatrix(resliceAxes);
        if(!use_image_interpolation)
            actor->InterpolateOff();

        // add the actor to the renderer's scene
        pRenderer->AddActor(actor);

        // also add a scalar bar to show how the colors correspond to values
        {
            vtkSmartPointer<vtkScalarBarActor> scalar_bar = vtkSmartPointer<vtkScalarBarActor>::New();
            scalar_bar->SetLookupTable(lut);

            pRenderer->AddActor2D(scalar_bar);
        }
    }
}

// ---------------------------------------------------------------------

void ImageRD::SaveStartingPattern()
{
    this->starting_pattern->DeepCopy(this->GetImage());
}

// ---------------------------------------------------------------------

void ImageRD::RestoreStartingPattern()
{
    this->CopyFromImage(this->starting_pattern);
    this->SetTimestepsTaken(0);
}

// ---------------------------------------------------------------------

void ImageRD::SetDimensions(int x, int y, int z)
{
    this->AllocateImages(x,y,z,this->GetNumberOfChemicals());
}

// ---------------------------------------------------------------------

void ImageRD::SetNumberOfChemicals(int n)
{
    this->AllocateImages(this->GetX(),this->GetY(),this->GetZ(),n);
}

// ---------------------------------------------------------------------

void ImageRD::SetDimensionsAndNumberOfChemicals(int x,int y,int z,int nc)
{
    this->AllocateImages(x,y,z,nc);
}

// ---------------------------------------------------------------------

int ImageRD::GetNumberOfCells() const
{
	return this->images.front()->GetDimensions()[0] * 
		this->images.front()->GetDimensions()[1] * 
		this->images.front()->GetDimensions()[2];
}

// ---------------------------------------------------------------------

void ImageRD::GetAsMesh(vtkPolyData *out, const Properties &render_settings) const
{
    bool use_image_interpolation = render_settings.GetProperty("use_image_interpolation").GetBool();
    int iActiveChemical = IndexFromChemicalName(render_settings.GetProperty("active_chemical").GetChemical());
    float contour_level = render_settings.GetProperty("contour_level").GetFloat();

    float low = render_settings.GetProperty("low").GetFloat();
    float high = render_settings.GetProperty("high").GetFloat();
    float vertical_scale_1D = render_settings.GetProperty("vertical_scale_1D").GetFloat();
    float vertical_scale_2D = render_settings.GetProperty("vertical_scale_2D").GetFloat();

    switch(this->GetArenaDimensionality())
    {
        case 1:
            {
                float scaling = vertical_scale_1D / (high-low); // vertical_scale gives the height of the graph in worldspace units

                vtkSmartPointer<vtkImageDataGeometryFilter> plane = vtkSmartPointer<vtkImageDataGeometryFilter>::New();
                plane->SetInput(this->GetImage(iActiveChemical));
                vtkSmartPointer<vtkWarpScalar> warp = vtkSmartPointer<vtkWarpScalar>::New();
                warp->SetInputConnection(plane->GetOutputPort());
                warp->SetScaleFactor(-scaling);
                warp->Update();
                out->DeepCopy(warp->GetOutput());
            }
            break;
        case 2:
            {
                float scaling = vertical_scale_2D / (high-low); // vertical_scale gives the height of the graph in worldspace units

                vtkSmartPointer<vtkImageDataGeometryFilter> plane = vtkSmartPointer<vtkImageDataGeometryFilter>::New();
                plane->SetInput(this->GetImage(iActiveChemical));
                vtkSmartPointer<vtkWarpScalar> warp = vtkSmartPointer<vtkWarpScalar>::New();
                warp->SetInputConnection(plane->GetOutputPort());
                warp->SetScaleFactor(scaling);
                vtkSmartPointer<vtkPolyDataNormals> normals = vtkSmartPointer<vtkPolyDataNormals>::New();
                normals->SetInputConnection(warp->GetOutputPort());
                normals->SplittingOff();
                normals->Update();
                out->DeepCopy(normals->GetOutput());
            }
            break;
        case 3:
            if(use_image_interpolation)
            {
                // turns the 3d grid of sampled values into a polygon mesh for rendering,
                // by making a surface that contours the volume at a specified level    
                vtkSmartPointer<vtkContourFilter> surface = vtkSmartPointer<vtkContourFilter>::New();
                surface->SetInput(this->GetImage(iActiveChemical));
                surface->SetValue(0, contour_level);
                surface->Update();
                out->DeepCopy(surface->GetOutput());
            }
            else
            {
                // render as cubes, Minecraft-style
                vtkImageData *image = this->GetImage(iActiveChemical);
                int *extent = image->GetExtent();

                vtkSmartPointer<vtkImageWrapPad> pad = vtkSmartPointer<vtkImageWrapPad>::New();
                pad->SetInput(image);
                pad->SetOutputWholeExtent(extent[0],extent[1]+1,extent[2],extent[3]+1,extent[4],extent[5]+1);
                pad->Update();
                pad->GetOutput()->GetCellData()->SetScalars(image->GetPointData()->GetScalars()); // a non-pipelined operation

                vtkSmartPointer<vtkThreshold> threshold = vtkSmartPointer<vtkThreshold>::New();
                threshold->SetInputConnection(pad->GetOutputPort());
                threshold->SetInputArrayToProcess(0, 0, 0,
                    vtkDataObject::FIELD_ASSOCIATION_CELLS,
                    vtkDataSetAttributes::SCALARS);
                threshold->ThresholdByUpper(contour_level);

                vtkSmartPointer<vtkTransform> transform = vtkSmartPointer<vtkTransform>::New();
                transform->Translate (-.5, -.5, -.5);
                vtkSmartPointer<vtkTransformFilter> transformModel = vtkSmartPointer<vtkTransformFilter>::New();
                transformModel->SetTransform(transform);
                transformModel->SetInputConnection(threshold->GetOutputPort());

                vtkSmartPointer<vtkGeometryFilter> geometry = vtkSmartPointer<vtkGeometryFilter>::New();
                geometry->SetInputConnection(transformModel->GetOutputPort());
                geometry->Update();

                out->DeepCopy(geometry->GetOutput());
            }
            break;
    }
}

// ---------------------------------------------------------------------

void ImageRD::SaveFile(const char* filename,const Properties& render_settings) const
{
    // convert the image to named arrays
    vtkSmartPointer<vtkImageData> im = vtkSmartPointer<vtkImageData>::New();
    im->DeepCopy(this->images.front());
    im->GetPointData()->SetScalars(NULL);
    for(int iChem=0;iChem<this->GetNumberOfChemicals();iChem++)
    {
        vtkSmartPointer<vtkFloatArray> fa = vtkSmartPointer<vtkFloatArray>::New();
        fa->DeepCopy(this->images[iChem]->GetPointData()->GetScalars());
        fa->SetName(GetChemicalName(iChem).c_str());
        im->GetPointData()->AddArray(fa);
    }

    vtkSmartPointer<RD_XMLImageWriter> iw = vtkSmartPointer<RD_XMLImageWriter>::New();
    iw->SetSystem(this);
    iw->SetRenderSettings(&render_settings);
    iw->SetFileName(filename);
    iw->SetInput(im);
    iw->Write();
}

// --------------------------------------------------------------------------------

vtkStandardNewMacro(RD_XMLImageWriter);

// --------------------------------------------------------------------------------

void RD_XMLImageWriter::SetSystem(const ImageRD* rd_system) 
{ 
    this->system = rd_system; 
}

// --------------------------------------------------------------------------------

int RD_XMLImageWriter::WritePrimaryElement(ostream& os,vtkIndent indent)
{
    vtkSmartPointer<vtkXMLDataElement> xml = this->system->GetAsXML();
    xml->AddNestedElement(this->render_settings->GetAsXML());
    xml->PrintXML(os,indent);
    return vtkXMLImageDataWriter::WritePrimaryElement(os,indent);
}

// --------------------------------------------------------------------------------

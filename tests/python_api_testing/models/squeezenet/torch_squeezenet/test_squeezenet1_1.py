
from pathlib import Path
import sys
f = f"{Path(__file__).parent}"
sys.path.append(f"{f}")
sys.path.append(f"{f}/..")
sys.path.append(f"{f}/../..")
sys.path.append(f"{f}/../../..")
sys.path.append(f"{f}/../../../..")

import torch
from torchvision import models
import pytest
from loguru import logger
from libs import tt_lib as ttl
from tqdm import tqdm
from imagenet import prep_ImageNet
from python_api_testing.sweep_tests.comparison_funcs import comp_allclose_and_pcc, comp_pcc
from squeezenet import squeezenet1_1


_batch_size = 1

@pytest.mark.parametrize("fuse_ops", [False, True], ids=['Not Fused', "Ops Fused"])
def test_squeezenet1_inference(fuse_ops):
    batch_size = _batch_size
    with torch.no_grad():

        torch_squeezenet = models.squeezenet1_1(weights=models.SqueezeNet1_1_Weights.IMAGENET1K_V1)

        torch_squeezenet.eval()

        state_dict = torch_squeezenet.state_dict()

        tt_squeezenet = squeezenet1_1(state_dict)
        tt_squeezenet.eval()

        if fuse_ops:
            modules_to_fuse = [['features.0', 'features.1'], ['classifier.1', 'classifier.2']]
            fire_indices = [3, 4, 6, 7, 10, 11, 12]
            fire_1 = [[f"features.{ind}.squeeze", f"features.{ind}.squeeze_activation", ] for ind in fire_indices]
            fire_2 = [[f"features.{ind}.expand1x1", f"features.{ind}.expand1x1_activation", ] for ind in fire_indices]
            fire_3 = [[f"features.{ind}.expand3x3", f"features.{ind}.expand3x3_activation", ] for ind in fire_indices]
            modules_to_fuse.extend(fire_1)
            modules_to_fuse.extend(fire_2)
            modules_to_fuse.extend(fire_3)

            tt_squeezenet = torch.ao.quantization.fuse_modules(tt_squeezenet, modules_to_fuse)

        dataloader = prep_ImageNet(batch_size = batch_size)
        for i, (images, targets, _, _, _) in enumerate(tqdm(dataloader)):
            torch_output = torch_squeezenet(images).unsqueeze(1).unsqueeze(1)
            tt_output = tt_squeezenet(images)

            passing = comp_pcc(torch_output, tt_output)

            assert passing[0], passing[1:]

            break
    logger.info(f"vgg16 PASSED {passing[1]}")

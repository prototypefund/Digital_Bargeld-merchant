from setuptools import setup, find_packages
setup(name='talermerchantmitm',
      version='0.0',
      description='Layer generating errors for testing',
      url='git://taler.net/merchant',
      author='Marcello Stanisci',
      author_email='marcello.stanisci@inria.fr',
      license='GPL',
      packages=find_packages(),
      install_requires=["Flask>=0.10"],
      zip_safe=False)
